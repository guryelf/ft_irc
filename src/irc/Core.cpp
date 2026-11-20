#include "irc/Core.hpp"

#include "Limits.hpp"
#include "irc/Text.hpp"

namespace
{

// Numerics are always sent as three digits: 1 -> "001".
std::string formatNumeric(Numeric code)
{
	std::string digits(3, '0');
	int value = static_cast<int>(code);

	for (int i = 2; i >= 0; --i)
	{
		digits[i] = static_cast<char>('0' + value % 10);
		value /= 10;
	}
	return digits;
}

// gmtime/strftime only compute: no system call, no time zone file.
std::string formatUtc(std::time_t when)
{
	char text[32];
	const std::tm* utc = std::gmtime(&when);

	if (utc == NULL || std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S UTC", utc) == 0)
		return "unknown";
	return text;
}

std::string closingLink(const Client& client, const std::string& reason)
{
	return "ERROR :Closing Link: " + client.host + " (" + reason + ")";
}

}

// ---------------------------------------------------------------- CommandSpec

Core::CommandSpec::CommandSpec()
	: handler(NULL), minParams(0), needsRegistration(true)
{
}

Core::CommandSpec::CommandSpec(Handler commandHandler, std::size_t requiredParams, bool registeredOnly)
	: handler(commandHandler), minParams(requiredParams), needsRegistration(registeredOnly)
{
}

// ---------------------------------------------------------------- entry points

// The command table: shared checks (parameter count, registration) are data
// here and run once in dispatch(), not repeated in every handler.
Core::Core(const std::string& password, std::time_t startedAt)
	: password_(password), createdAt_(formatUtc(startedAt)), now_(startedAt)
{
	// NICK, PING, PRIVMSG and NOTICE take 0 so their handlers can send the
	// specific error (431, 409, 411, 412) instead of ERR_NEEDMOREPARAMS.
	commands_["CAP"] = CommandSpec(&Core::cmdCap, 1, false);
	commands_["PASS"] = CommandSpec(&Core::cmdPass, 1, false);
	commands_["NICK"] = CommandSpec(&Core::cmdNick, 0, false);
	commands_["USER"] = CommandSpec(&Core::cmdUser, 4, false);
	commands_["PING"] = CommandSpec(&Core::cmdPing, 0, false);
	commands_["PONG"] = CommandSpec(&Core::cmdPong, 0, false);
	commands_["QUIT"] = CommandSpec(&Core::cmdQuit, 0, false);
	commands_["JOIN"] = CommandSpec(&Core::cmdJoin, 1, true);
	commands_["PART"] = CommandSpec(&Core::cmdPart, 1, true);
	commands_["TOPIC"] = CommandSpec(&Core::cmdTopic, 1, true);
	commands_["NAMES"] = CommandSpec(&Core::cmdNames, 0, true);
	commands_["WHO"] = CommandSpec(&Core::cmdWho, 0, true);
	commands_["PRIVMSG"] = CommandSpec(&Core::cmdPrivmsg, 0, true);
	commands_["NOTICE"] = CommandSpec(&Core::cmdNotice, 0, true);
	commands_["KICK"] = CommandSpec(&Core::cmdKick, 2, true);
	commands_["INVITE"] = CommandSpec(&Core::cmdInvite, 2, true);
	commands_["MODE"] = CommandSpec(&Core::cmdMode, 1, true);
}

void Core::onConnect(ClientId id, const std::string& host, std::time_t now)
{
	now_ = now;
	clients_.insert(std::make_pair(id, Client(id, host, now)));
}

void Core::onData(ClientId id, const std::string& bytes, std::time_t now)
{
	Client* client = findClient(id);

	now_ = now;
	if (client == NULL)
		return;
	// Any input proves the client is alive, PONG or not.
	client->lastActivity = now;
	client->pingPending = false;
	client->inbuf += bytes;
	processInput(id, now);
}

void Core::onClose(ClientId id, const std::string& reason)
{
	removeClient(id, reason);
}

void Core::onTick(std::time_t now)
{
	// Collect ids first: processing input can remove clients from the map.
	std::vector<ClientId> throttled;

	now_ = now;
	for (ClientMap::const_iterator it = clients_.begin(); it != clients_.end(); ++it)
	{
		if (it->second.throttled)
			throttled.push_back(it->first);
	}
	for (std::vector<ClientId>::size_type i = 0; i < throttled.size(); ++i)
		processInput(throttled[i], now);
	checkLiveness(now);
}

void Core::onShutdown()
{
	// Everyone is leaving at once, so no QUIT is relayed between clients.
	for (ClientMap::const_iterator it = clients_.begin(); it != clients_.end(); ++it)
	{
		sendLine(it->first, closingLink(it->second, "Server shutting down"));
		outbox_.push_back(Effect(Effect::CLOSE, it->first, "Server shutting down"));
	}
	clients_.clear();
	nicks_.clear();
	channels_.clear();
}

bool Core::wantsInput(ClientId id) const
{
	const ClientMap::const_iterator it = clients_.find(id);

	return it != clients_.end() && !it->second.throttled;
}

void Core::takeEffects(std::vector<Effect>& out)
{
	out.clear();
	out.swap(outbox_);
}

// ---------------------------------------------------------------- input pipeline

// Cuts complete lines out of the client's input buffer and runs them.
// CR and LF both end a line, so a stray CR can never smuggle a second line
// into a message relayed to other clients. Empty lines are skipped for free;
// every other line costs one flood token.
void Core::processInput(ClientId id, std::time_t now)
{
	Client* client = findClient(id);

	if (client == NULL)
		return;
	refillFlood(*client, now);
	client->throttled = false;
	// Look the client up again on every line: a command such as QUIT removes it.
	while ((client = findClient(id)) != NULL)
	{
		const std::string::size_type end = client->inbuf.find_first_of("\r\n");
		if (end == std::string::npos)
		{
			// A valid line is at most MAX_LINE - 2 bytes before its line ending,
			// so a longer unfinished line can never become valid: drop it now
			// instead of buffering it.
			if (client->inbuf.size() > limits::MAX_LINE - 2)
			{
				if (!client->discarding)
					reply(*client, ERR_INPUTTOOLONG, "", "Input line was too long");
				client->discarding = true;
				client->inbuf.clear();
			}
			return;
		}
		if (client->discarding)
		{
			// This ends the oversized line that was being dropped.
			client->inbuf.erase(0, end + 1);
			client->discarding = false;
			continue;
		}
		if (end == 0)
		{
			client->inbuf.erase(0, 1);
			continue;
		}
		if (client->floodTokens == 0)
		{
			// The line stays in inbuf; onTick resumes once tokens are back.
			client->throttled = true;
			return;
		}
		--client->floodTokens;
		const std::string line = client->inbuf.substr(0, end);
		client->inbuf.erase(0, end + 1);
		if (line.size() > limits::MAX_LINE - 2)
		{
			reply(*client, ERR_INPUTTOOLONG, "", "Input line was too long");
			continue;
		}
		Message msg;
		if (Message::parse(line, msg))
			dispatch(*client, msg);
	}
}

// Token bucket: FLOOD_RATE tokens per second, at most FLOOD_BURST stored.
void Core::refillFlood(Client& client, std::time_t now)
{
	if (now <= client.floodRefilledAt)
	{
		// Same second, or the clock went backwards: restart counting from now.
		client.floodRefilledAt = now;
		return;
	}
	const std::time_t elapsed = now - client.floodRefilledAt;
	client.floodRefilledAt = now;
	if (elapsed >= static_cast<std::time_t>(limits::FLOOD_BURST))
	{
		client.floodTokens = limits::FLOOD_BURST;
		return;
	}
	const unsigned tokens = client.floodTokens + static_cast<unsigned>(elapsed) * limits::FLOOD_RATE;
	client.floodTokens = (tokens > limits::FLOOD_BURST) ? limits::FLOOD_BURST : tokens;
}

// Runs the checks every command shares, then the handler. A handler reports a
// protocol error by throwing IrcError; it is turned into a numeric reply here
// and never travels further.
void Core::dispatch(Client& client, const Message& msg)
{
	const ClientId id = client.id;

	try
	{
		const CommandMap::const_iterator it = commands_.find(msg.command);
		if (it == commands_.end())
		{
			if (!client.registered)
				throw IrcError(ERR_NOTREGISTERED, "", "You have not registered");
			throw IrcError(ERR_UNKNOWNCOMMAND, asParam(msg.command), "Unknown command");
		}
		const CommandSpec& spec = it->second;
		if (spec.needsRegistration && !client.registered)
			throw IrcError(ERR_NOTREGISTERED, "", "You have not registered");
		if (msg.params.size() < spec.minParams)
			throw IrcError(ERR_NEEDMOREPARAMS, msg.command, "Not enough parameters");
		(this->*spec.handler)(client, msg);
	}
	catch (const IrcError& error)
	{
		// The handler may have removed the client before throwing.
		Client* stillHere = findClient(id);
		if (stillHere != NULL)
			replyError(*stillHere, error);
	}
}

// Unregistered clients get REG_TIMEOUT seconds to finish PASS/NICK/USER.
// A registered client silent for PING_IDLE seconds gets a PING, and is
// dropped if nothing at all arrives within PING_TIMEOUT more seconds.
// A throttled client is visibly sending, so it is left alone.
void Core::checkLiveness(std::time_t now)
{
	std::vector<std::pair<ClientId, std::string> > expired;

	for (ClientMap::iterator it = clients_.begin(); it != clients_.end(); ++it)
	{
		Client& client = it->second;
		if (client.throttled)
			continue;
		if (!client.registered)
		{
			if (now - client.connectedAt >= limits::REG_TIMEOUT)
				expired.push_back(std::make_pair(client.id, std::string("Registration timeout")));
		}
		else if (client.pingPending)
		{
			if (now - client.pingSentAt >= limits::PING_TIMEOUT)
				expired.push_back(std::make_pair(client.id,
					"Ping timeout: " + toString(static_cast<unsigned long>(limits::PING_TIMEOUT)) + " seconds"));
		}
		else if (now - client.lastActivity >= limits::PING_IDLE)
		{
			sendLine(client.id, std::string("PING :") + limits::SERVER_NAME);
			client.pingPending = true;
			client.pingSentAt = now;
		}
	}
	// Disconnect after the scan: it erases clients from the map being walked.
	for (std::vector<std::pair<ClientId, std::string> >::size_type i = 0; i < expired.size(); ++i)
		disconnect(expired[i].first, expired[i].second);
}

// ---------------------------------------------------------------- state

Client* Core::findClient(ClientId id)
{
	const ClientMap::iterator it = clients_.find(id);

	return (it == clients_.end()) ? NULL : &it->second;
}

// Keeps the nick index in step with Client::nick.
void Core::setNick(Client& client, const std::string& nick)
{
	if (!client.nick.empty())
	{
		const NickMap::iterator old = nicks_.find(ircLower(client.nick));
		if (old != nicks_.end() && old->second == client.id)
			nicks_.erase(old);
	}
	nicks_[ircLower(nick)] = client.id;
	client.nick = nick;
}

std::set<ClientId> Core::peersOf(const Client& client) const
{
	std::set<ClientId> peers;

	for (std::set<std::string>::const_iterator name = client.channels.begin(); name != client.channels.end(); ++name)
	{
		const ChannelMap::const_iterator channel = channels_.find(*name);
		if (channel != channels_.end())
			peers.insert(channel->second.members().begin(), channel->second.members().end());
	}
	peers.erase(client.id);
	return peers;
}

void Core::removeClient(ClientId id, const std::string& quitReason)
{
	const ClientMap::iterator found = clients_.find(id);

	if (found == clients_.end())
		return;
	const Client& client = found->second;

	// A set, so a peer sharing several channels gets the QUIT only once.
	const std::set<ClientId> peers = peersOf(client);
	for (std::set<std::string>::const_iterator name = client.channels.begin(); name != client.channels.end(); ++name)
	{
		const ChannelMap::iterator channel = channels_.find(*name);
		if (channel == channels_.end())
			continue;
		channel->second.removeMember(id);
		if (channel->second.empty())
			channels_.erase(channel);
	}
	sendToAll(peers, ":" + client.mask() + " QUIT :" + quitReason);

	// Pending invites name this id; ids are never reused, so this only frees memory.
	for (ChannelMap::iterator channel = channels_.begin(); channel != channels_.end(); ++channel)
		channel->second.uninvite(id);

	const NickMap::iterator nick = nicks_.find(ircLower(client.nick));
	if (nick != nicks_.end() && nick->second == id)
		nicks_.erase(nick);
	clients_.erase(found);
}

Client* Core::findClientByNick(const std::string& nick)
{
	const NickMap::const_iterator it = nicks_.find(ircLower(nick));

	if (it == nicks_.end())
		return NULL;
	// The index also holds nicks reserved by clients still registering.
	Client* client = findClient(it->second);
	return (client != NULL && client->registered) ? client : NULL;
}

Channel* Core::findChannel(const std::string& name)
{
	const ChannelMap::iterator it = channels_.find(ircLower(name));

	return (it == channels_.end()) ? NULL : &it->second;
}

Channel& Core::requireChannel(const std::string& name)
{
	Channel* channel = findChannel(name);

	if (channel == NULL)
		throw IrcError(ERR_NOSUCHCHANNEL, asParam(name), "No such channel");
	return *channel;
}

void Core::requireMember(const Channel& channel, const Client& client) const
{
	if (!channel.isMember(client.id))
		throw IrcError(ERR_NOTONCHANNEL, channel.name, "You're not on that channel");
}

void Core::requireOperator(const Channel& channel, const Client& client) const
{
	if (!channel.isOperator(client.id))
		throw IrcError(ERR_CHANOPRIVSNEEDED, channel.name, "You're not channel operator");
}

void Core::leaveChannel(Client& client, const std::string& channelKey)
{
	client.channels.erase(channelKey);
	const ChannelMap::iterator it = channels_.find(channelKey);
	if (it == channels_.end())
		return;
	it->second.removeMember(client.id);
	if (it->second.empty())
		channels_.erase(it);
}

void Core::disconnect(ClientId id, const std::string& reason)
{
	const Client* client = findClient(id);

	if (client == NULL)
		return;
	// ERROR is queued before CLOSE, so the shell flushes it before closing.
	sendLine(id, closingLink(*client, reason));
	removeClient(id, reason);
	outbox_.push_back(Effect(Effect::CLOSE, id, reason));
}

// ---------------------------------------------------------------- output

void Core::sendLine(ClientId id, const std::string& line)
{
	// RFC 2812: a message is at most 512 bytes including CRLF. Longer lines are cut.
	const std::string::size_type maxContent = limits::MAX_LINE - 2;

	if (line.size() > maxContent)
		outbox_.push_back(Effect(Effect::SEND, id, line.substr(0, maxContent) + "\r\n"));
	else
		outbox_.push_back(Effect(Effect::SEND, id, line + "\r\n"));
}

void Core::sendToAll(const std::set<ClientId>& ids, const std::string& line)
{
	for (std::set<ClientId>::const_iterator id = ids.begin(); id != ids.end(); ++id)
		sendLine(*id, line);
}

void Core::broadcast(const Channel& channel, const std::string& line)
{
	sendToAll(channel.members(), line);
}

void Core::broadcastExcept(const Channel& channel, const std::string& line, ClientId except)
{
	for (std::set<ClientId>::const_iterator id = channel.members().begin(); id != channel.members().end(); ++id)
	{
		if (*id != except)
			sendLine(*id, line);
	}
}

// Sends ":<server> <code> <nick> <params>", for numerics without trailing text.
void Core::reply(const Client& client, Numeric code, const std::string& params)
{
	sendLine(client.id, std::string(":") + limits::SERVER_NAME + " " + formatNumeric(code) + " "
		+ client.displayNick() + " " + params);
}

// Sends ":<server> <code> <nick> [<params>] :<text>".
void Core::reply(const Client& client, Numeric code, const std::string& params, const std::string& text)
{
	std::string line = std::string(":") + limits::SERVER_NAME + " " + formatNumeric(code) + " " + client.displayNick();

	if (!params.empty())
		line += " " + params;
	sendLine(client.id, line + " :" + text);
}

void Core::replyError(const Client& client, const IrcError& error)
{
	reply(client, error.code(), error.params(), error.text());
}

void Core::sendTopic(const Client& client, const Channel& channel)
{
	reply(client, RPL_TOPIC, channel.name, channel.topic);
	reply(client, RPL_TOPICWHOTIME, channel.name + " " + channel.topicSetter + " "
		+ toString(static_cast<unsigned long>(channel.topicSetAt)));
}

// "@" marks operators. Names are spread over as many 353 lines as needed so
// that none exceeds the 512-byte message limit.
void Core::sendNames(const Client& client, const Channel& channel)
{
	const std::string head = std::string(":") + limits::SERVER_NAME + " " + formatNumeric(RPL_NAMREPLY) + " "
		+ client.displayNick() + " = " + channel.name + " :";
	std::string names;

	for (std::set<ClientId>::const_iterator id = channel.members().begin(); id != channel.members().end(); ++id)
	{
		const Client* member = findClient(*id);
		if (member == NULL)
			continue;
		const std::string entry = (channel.isOperator(*id) ? "@" : "") + member->nick;
		if (!names.empty() && head.size() + names.size() + 1 + entry.size() > limits::MAX_LINE - 2)
		{
			sendLine(client.id, head + names);
			names.clear();
		}
		if (!names.empty())
			names += ' ';
		names += entry;
	}
	if (!names.empty())
		sendLine(client.id, head + names);
	reply(client, RPL_ENDOFNAMES, channel.name, "End of /NAMES list");
}
