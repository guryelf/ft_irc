#include "net/Reactor.hpp"

#include <cerrno>
#include <cstring>
#include <exception>
#include <stdexcept>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "Limits.hpp"
#include "Log.hpp"
#include "irc/Text.hpp"

namespace
{

// Mark the listening socket and stderr in the poll set. Client ids start at
// 1 and only go up, so neither value can ever name a client.
const ClientId LISTENER = 0;
const ClientId LOG_OUTPUT = static_cast<ClientId>(-1);

// Poll timeout while draining at shutdown: short, so the flush deadline is kept.
const int DRAIN_POLL_TIMEOUT_MS = 100;

// Readable form of a peer address. "::ffff:1.2.3.4" (IPv4 through a
// dual-stack socket) becomes "1.2.3.4". A leading ':' gets a '0' in front:
// a host such as "::1" would otherwise read as a trailing parameter in replies.
std::string hostFromAddress(const struct sockaddr_storage& address)
{
	char text[INET6_ADDRSTRLEN];
	const char* result = NULL;

	if (address.ss_family == AF_INET)
	{
		const struct sockaddr_in* ipv4 = reinterpret_cast<const struct sockaddr_in*>(&address);
		result = inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text));
	}
	else if (address.ss_family == AF_INET6)
	{
		const struct sockaddr_in6* ipv6 = reinterpret_cast<const struct sockaddr_in6*>(&address);
		result = inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text));
	}
	std::string host = (result != NULL) ? result : "unknown";
	if (host.compare(0, 7, "::ffff:") == 0 && host.find('.') != std::string::npos)
		host.erase(0, 7);
	if (!host.empty() && host[0] == ':')
		host.insert(0, "0");
	return host;
}

// Opens a non-blocking listening socket on every address of one family.
// Returns -1 and describes the failed step in error.
int openListeningSocket(int family, const std::string& port, std::string& error)
{
	struct addrinfo hints;
	struct addrinfo* list = NULL;

	std::memset(&hints, 0, sizeof(hints));
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;
	if (getaddrinfo(NULL, port.c_str(), &hints, &list) != 0 || list == NULL)
	{
		error = "getaddrinfo failed";
		return -1;
	}

	const int on = 1;
	const int off = 0;
	const char* failedStep = "socket";
	int fd = socket(list->ai_family, list->ai_socktype, list->ai_protocol);
	if (fd >= 0)
	{
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
			failedStep = "setsockopt(SO_REUSEADDR)";
		else if (family == AF_INET6 && setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) < 0)
			failedStep = "setsockopt(IPV6_V6ONLY)";
		else if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
			failedStep = "fcntl";
		else if (bind(fd, list->ai_addr, list->ai_addrlen) < 0)
			failedStep = "bind";
		else if (listen(fd, SOMAXCONN) < 0)
			failedStep = "listen";
		else
			failedStep = NULL;
	}
	if (failedStep != NULL)
	{
		// Read errno before close(), which may overwrite it.
		error = std::string(failedStep) + ": " + std::strerror(errno);
		if (fd >= 0)
			close(fd);
		fd = -1;
	}
	freeaddrinfo(list);
	return fd;
}

// stderr: an error or hangup means nobody can read the log any more.
void serviceLog(short revents)
{
	if (revents & (POLLERR | POLLHUP | POLLNVAL))
		logDisable();
	else if (revents & POLLOUT)
		logWriteSome();
}

void addPollEntry(std::vector<struct pollfd>& fds, std::vector<ClientId>& ids,
	int fd, short events, ClientId id)
{
	struct pollfd entry;

	entry.fd = fd;
	entry.events = events;
	entry.revents = 0;
	fds.push_back(entry);
	ids.push_back(id);
}

}

// ---------------------------------------------------------------- Connection

Reactor::Connection::Connection(int socketFd, const std::string& peerHost)
	: fd(socketFd),
	  host(peerHost),
	  coreKnows(true),
	  closing(false),
	  closeDeadline(0),
	  dead(false),
	  deadReason("")
{
}

// ---------------------------------------------------------------- lifetime

Reactor::Reactor(Core& core, const std::string& port)
	: core_(core), listenFd_(-1), nextId_(1)
{
	std::string ipv6Error;
	listenFd_ = openListeningSocket(AF_INET6, port, ipv6Error);
	if (listenFd_ >= 0)
		return;
	std::string ipv4Error;
	listenFd_ = openListeningSocket(AF_INET, port, ipv4Error);
	if (listenFd_ < 0)
		throw std::runtime_error("cannot listen on port " + port + ": " + ipv4Error);
	logWarn("IPv6 unavailable (" + ipv6Error + "), listening on IPv4 only");
}

Reactor::~Reactor()
{
	for (ConnectionMap::iterator it = connections_.begin(); it != connections_.end(); ++it)
		close(it->second.fd);
	if (listenFd_ >= 0)
		close(listenFd_);
}

// ---------------------------------------------------------------- event loop

void Reactor::run(volatile std::sig_atomic_t& stop)
{
	std::vector<struct pollfd> fds;
	std::vector<ClientId> ids;		// ids[i] says who owns fds[i]
	bool draining = false;
	bool stoppedLogged = false;
	std::time_t drainDeadline = 0;

	for (;;)
	{
		if (stop && !draining)
		{
			draining = true;
			drainDeadline = std::time(NULL) + limits::SHUTDOWN_FLUSH_SECONDS;
			beginShutdown();
		}
		if (draining)
		{
			if (connections_.empty() && !stoppedLogged)
			{
				logInfo("server stopped");
				stoppedLogged = true;
			}
			// Done once every client is gone and the log is written out, or
			// when the flush time is up (whatever is still queued is lost).
			if ((connections_.empty() && !logPending()) || std::time(NULL) >= drainDeadline)
				return;
		}

		buildPollSet(fds, ids);
		// The program's only poll() call: accept, read and write (sockets and
		// the log on stderr) all wait here.
		const int ready = poll(fds.empty() ? NULL : &fds[0], fds.size(),
			draining ? DRAIN_POLL_TIMEOUT_MS : limits::POLL_TIMEOUT_MS);
		if (ready < 0)
		{
			// This errno comes from poll(); it is never read after recv()/send().
			// EINTR means a signal arrived: the loop re-checks stop.
			if (errno == EINTR)
				continue;
			throw std::runtime_error(std::string("poll: ") + std::strerror(errno));
		}

		const std::time_t now = std::time(NULL);
		try
		{
			for (std::vector<struct pollfd>::size_type i = 0; i < fds.size(); ++i)
			{
				if (fds[i].revents == 0)
					continue;
				if (ids[i] == LISTENER)
					acceptClient(now);
				else if (ids[i] == LOG_OUTPUT)
					serviceLog(fds[i].revents);
				else
					handleEvents(ids[i], fds[i].revents, now);
			}
			if (!draining)
			{
				core_.onTick(now);
				applyEffects(now);
			}
		}
		catch (const std::exception& e)
		{
			// Not tied to one client (handleEvents catches those): log and keep serving.
			logError(std::string("event loop: ") + e.what());
		}
		reap(now);
	}
}

// POLLIN only while the core wants input (not throttled, not closing).
// POLLOUT only while output is queued: asking for it otherwise would wake
// poll() at once on every iteration.
void Reactor::buildPollSet(std::vector<struct pollfd>& fds, std::vector<ClientId>& ids) const
{
	fds.clear();
	ids.clear();
	if (listenFd_ >= 0)
		addPollEntry(fds, ids, listenFd_, POLLIN, LISTENER);
	if (logPending())
		addPollEntry(fds, ids, LOG_FD, POLLOUT, LOG_OUTPUT);
	for (ConnectionMap::const_iterator it = connections_.begin(); it != connections_.end(); ++it)
	{
		const Connection& conn = it->second;
		if (conn.dead)
			continue;
		short events = 0;
		if (!conn.closing && core_.wantsInput(it->first))
			events |= POLLIN;
		if (!conn.sendq.empty())
			events |= POLLOUT;
		addPollEntry(fds, ids, conn.fd, events, it->first);
	}
}

// One accept() per readiness event; the listener stays readable while more
// connections wait, so poll() reports it again on the next iteration.
void Reactor::acceptClient(std::time_t now)
{
	struct sockaddr_storage address;
	socklen_t length = sizeof(address);
	const int fd = accept(listenFd_, reinterpret_cast<struct sockaddr*>(&address), &length);

	if (fd < 0)
		return;
	if (connections_.size() >= limits::MAX_CLIENTS)
	{
		close(fd);
		logWarn("connection refused: server full");
		return;
	}
	// Linux does not copy O_NONBLOCK from the listener to accepted sockets.
	if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0)
	{
		close(fd);
		return;
	}
	const ClientId id = nextId_++;
	const std::string host = hostFromAddress(address);
	connections_.insert(std::make_pair(id, Connection(fd, host)));
	core_.onConnect(id, host, now);
	applyEffects(now);
	logInfo("client " + toString(id) + " connected from " + host);
}

void Reactor::handleEvents(ClientId id, short revents, std::time_t now)
{
	const ConnectionMap::iterator it = connections_.find(id);

	if (it == connections_.end() || it->second.dead)
		return;
	Connection& conn = it->second;
	try
	{
		if (revents & (POLLERR | POLLNVAL))
		{
			markDead(conn, "Connection error");
			return;
		}
		// Read before acting on POLLHUP: the peer may have sent a last
		// command (QUIT) just before hanging up.
		if (revents & POLLIN)
			readFrom(id, conn, now);
		else if (revents & POLLHUP)
			markDead(conn, "Connection closed");
		if (!conn.dead && (revents & POLLOUT))
			writeTo(conn);
	}
	catch (const std::exception&)
	{
		// Out of memory, or a bug, while serving this client: drop only this client.
		markDead(conn, "Internal error");
	}
}

void Reactor::readFrom(ClientId id, Connection& conn, std::time_t now)
{
	char buffer[limits::RECV_CHUNK];
	const ssize_t received = recv(conn.fd, buffer, sizeof(buffer), 0);

	// The return value alone decides: 0 is an orderly close, and -1 right
	// after poll() reported POLLIN is a real error. errno is not consulted.
	if (received <= 0)
	{
		markDead(conn, received == 0 ? "Connection closed" : "Read error");
		return;
	}
	core_.onData(id, std::string(buffer, static_cast<std::size_t>(received)), now);
	applyEffects(now);
}

void Reactor::writeTo(Connection& conn)
{
	const ssize_t sent = send(conn.fd, conn.sendq.data(), conn.sendq.size(), 0);

	// poll() reported POLLOUT, so -1 is a real error (SIGPIPE is ignored).
	// A partial send keeps the rest queued for the next POLLOUT.
	if (sent < 0)
	{
		markDead(conn, "Write error");
		return;
	}
	conn.sendq.erase(0, static_cast<std::size_t>(sent));
}

// Carries out what the core asked for. SEND only queues bytes: nothing is
// written here, because send() may only run after poll() reports POLLOUT.
void Reactor::applyEffects(std::time_t now)
{
	core_.takeEffects(effects_);
	for (std::vector<Effect>::size_type i = 0; i < effects_.size(); ++i)
	{
		const Effect& effect = effects_[i];
		const ConnectionMap::iterator it = connections_.find(effect.id);
		if (it == connections_.end())
			continue;
		Connection& conn = it->second;
		if (conn.dead)
			continue;
		if (effect.kind == Effect::SEND)
		{
			if (conn.closing)
				continue;
			if (conn.sendq.size() + effect.data.size() > limits::SENDQ_MAX)
			{
				markDead(conn, "SendQ exceeded");
				continue;
			}
			conn.sendq += effect.data;
		}
		else if (!conn.closing)
		{
			conn.coreKnows = false;
			conn.closing = true;
			conn.closeDeadline = now + limits::CLOSE_FLUSH_SECONDS;
			conn.closeReason = effect.data;
		}
	}
	effects_.clear();
}

void Reactor::markDead(Connection& conn, const char* reason)
{
	if (conn.dead)
		return;
	conn.dead = true;
	conn.deadReason = reason;
}

// Closes dead connections, and closing ones that flushed their output or ran
// out of time. Running only here, after all events of the iteration, means no
// fd is closed while this iteration's poll set still refers to it.
void Reactor::reap(std::time_t now)
{
	bool closedOne = true;

	while (closedOne)
	{
		closedOne = false;
		ConnectionMap::iterator it = connections_.begin();
		while (it != connections_.end())
		{
			const Connection& conn = it->second;
			const bool flushed = conn.closing && (conn.sendq.empty() || now >= conn.closeDeadline);
			if (!conn.dead && !flushed)
			{
				++it;
				continue;
			}
			const ClientId id = it->first;
			const bool tellCore = conn.coreKnows;
			const std::string reason = conn.dead ? conn.deadReason : conn.closeReason;
			close(conn.fd);
			connections_.erase(it++);
			logInfo("client " + toString(id) + " disconnected: " + reason);
			if (tellCore)
			{
				// The QUIT sent to peers can overflow a peer's sendq and mark
				// it dead, possibly behind this iterator: scan again.
				core_.onClose(id, reason);
				applyEffects(now);
				closedOne = true;
			}
		}
	}
}

void Reactor::beginShutdown()
{
	logInfo("shutting down");
	// Refuse new connections at once; existing ones get ERROR and a flush.
	close(listenFd_);
	listenFd_ = -1;
	core_.onShutdown();
	applyEffects(std::time(NULL));
}
