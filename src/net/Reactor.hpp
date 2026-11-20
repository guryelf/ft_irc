#ifndef REACTOR_HPP
#define REACTOR_HPP

#include <csignal>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include <poll.h>

#include "irc/Core.hpp"
#include "irc/Effect.hpp"

// The network shell: the only code that touches sockets.
//
// Each loop iteration makes the program's single poll() call over the
// listening socket and every client. Bytes read go to the Core; the Effects
// the Core returns become queued output (sent on POLLOUT) or closes.
// Connections are only ever closed in reap(), at the end of an iteration.
class Reactor
{
public:
	// Opens the listening socket (IPv6 dual-stack, else IPv4). Throws on failure.
	Reactor(Core& core, const std::string& port);
	~Reactor();

	// Serves clients until stop becomes non-zero, then sends ERROR to every
	// client, flushes for at most SHUTDOWN_FLUSH_SECONDS, and returns.
	void run(volatile std::sig_atomic_t& stop);

private:
	struct Connection
	{
		int fd;
		std::string host;
		std::string sendq;			// bytes waiting for POLLOUT
		bool coreKnows;				// false once the core asked to close it
		bool closing;				// flush sendq, then close (core-initiated)
		std::time_t closeDeadline;
		std::string closeReason;
		bool dead;					// close without flushing (shell-initiated)
		const char* deadReason;		// a literal: setting it never allocates

		Connection(int socketFd, const std::string& peerHost);
	};

	typedef std::map<ClientId, Connection> ConnectionMap;

	// Not copyable: it owns file descriptors.
	Reactor(const Reactor&);
	Reactor& operator=(const Reactor&);

	void buildPollSet(std::vector<struct pollfd>& fds, std::vector<ClientId>& ids) const;
	void acceptClient(std::time_t now);
	void handleEvents(ClientId id, short revents, std::time_t now);
	void readFrom(ClientId id, Connection& conn, std::time_t now);
	void writeTo(Connection& conn);
	void applyEffects(std::time_t now);
	void markDead(Connection& conn, const char* reason);
	void reap(std::time_t now);
	void beginShutdown();

	Core& core_;
	int listenFd_;
	ClientId nextId_;
	ConnectionMap connections_;
	std::vector<Effect> effects_;	// reused buffer for Core::takeEffects
};

#endif
