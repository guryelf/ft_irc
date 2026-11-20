// Connection registration (PASS, NICK, USER, CAP) and connection upkeep
// (PING, PONG, QUIT).

#include "irc/Core.hpp"

#include "Limits.hpp"
#include "irc/Text.hpp"

namespace
{

// The user name ends up in "nick!user@host" masks. '!' and '@' would make
// those masks ambiguous, so they are replaced; the length is capped.
std::string sanitizeUser(const std::string& user)
{
	std::string clean = user.substr(0, limits::USERLEN);

	for (std::string::size_type i = 0; i < clean.size(); ++i)
	{
		if (clean[i] == '!' || clean[i] == '@')
			clean[i] = '_';
	}
	return clean;
}

}

// Only enough of IRCv3 capability negotiation for clients that start with
// "CAP LS 302" (irssi, WeeChat, HexChat): no capability is offered, and
// registration waits until the client sends CAP END.
void Core::cmdCap(Client& client, const Message& msg)
{
	const std::string subcommand = asciiUpper(msg.params[0]);
	const std::string prefix = std::string(":") + limits::SERVER_NAME + " CAP " + client.displayNick() + " ";

	if (subcommand == "LS" || subcommand == "REQ")
	{
		if (!client.registered)
			client.capNegotiating = true;
		if (subcommand == "LS")
			sendLine(client.id, prefix + "LS :");
		else
			sendLine(client.id, prefix + "NAK :" + (msg.params.size() > 1 ? msg.params[1] : ""));
	}
	else if (subcommand == "LIST")
		sendLine(client.id, prefix + "LIST :");
	else if (subcommand == "END")
	{
		if (client.capNegotiating)
		{
			client.capNegotiating = false;
			tryCompleteRegistration(client);
		}
	}
	else
		throw IrcError(ERR_INVALIDCAPCMD, asParam(subcommand), "Invalid CAP command");
}

void Core::cmdPass(Client& client, const Message& msg)
{
	if (client.registered)
		throw IrcError(ERR_ALREADYREGISTERED, "", "You may not reregister");
	if (msg.params[0] != password_)
	{
		reply(client, ERR_PASSWDMISMATCH, "", "Password incorrect");
		disconnect(client.id, "Bad password");
		return;
	}
	client.passOk = true;
}

void Core::cmdNick(Client& client, const Message& msg)
{
	if (msg.params.empty() || msg.params[0].empty())
		throw IrcError(ERR_NONICKNAMEGIVEN, "", "No nickname given");
	const std::string& nick = msg.params[0];
	if (!isValidNick(nick))
		throw IrcError(ERR_ERRONEUSNICKNAME, asParam(nick), "Erroneous nickname");
	if (nick == client.nick)
		return;
	// Folded lookup: "Alice" and "alice", or "[a]" and "{a}", are the same nick.
	// A client may still change the case of its own nick.
	const NickMap::const_iterator owner = nicks_.find(ircLower(nick));
	if (owner != nicks_.end() && owner->second != client.id)
		throw IrcError(ERR_NICKNAMEINUSE, nick, "Nickname is already in use");

	if (!client.registered)
	{
		setNick(client, nick);
		tryCompleteRegistration(client);
		return;
	}
	// Everyone who can see this client learns the new nick once, itself included.
	const std::string line = ":" + client.mask() + " NICK :" + nick;
	std::set<ClientId> audience = peersOf(client);
	audience.insert(client.id);
	setNick(client, nick);
	sendToAll(audience, line);
}

void Core::cmdUser(Client& client, const Message& msg)
{
	if (client.registered)
		throw IrcError(ERR_ALREADYREGISTERED, "", "You may not reregister");
	client.user = sanitizeUser(msg.params[0]);
	client.realname = msg.params[3];
	tryCompleteRegistration(client);
}

void Core::cmdPing(Client& client, const Message& msg)
{
	if (msg.params.empty() || msg.params[0].empty())
		throw IrcError(ERR_NOORIGIN, "", "No origin specified");
	sendLine(client.id, std::string(":") + limits::SERVER_NAME + " PONG " + limits::SERVER_NAME + " :" + msg.params[0]);
}

// onData already counted the PONG as activity. The command exists so that
// it is not answered with ERR_UNKNOWNCOMMAND.
void Core::cmdPong(Client&, const Message&)
{
}

void Core::cmdQuit(Client& client, const Message& msg)
{
	const bool hasReason = !msg.params.empty() && !msg.params[0].empty();

	disconnect(client.id, hasReason ? "Quit: " + msg.params[0] : "Client Quit");
}

// Registration completes once NICK and USER are both known and CAP
// negotiation (if started) has ended. The password is checked at that point:
// PASS may come in any position as long as it comes before that.
// On failure the client is disconnected: callers must not touch it afterwards.
void Core::tryCompleteRegistration(Client& client)
{
	if (client.registered || client.nick.empty() || client.user.empty() || client.capNegotiating)
		return;
	if (!client.passOk)
	{
		reply(client, ERR_PASSWDMISMATCH, "", "Password required");
		disconnect(client.id, "Bad password");
		return;
	}
	client.registered = true;
	sendWelcome(client);
}

void Core::sendWelcome(const Client& client)
{
	const std::string server = limits::SERVER_NAME;
	const std::string version = limits::SERVER_VERSION;
	const std::string isupport =
		"CASEMAPPING=rfc1459 CHANLIMIT=#&:" + toString(limits::CHANLIMIT)
		+ " CHANMODES=,k,l,it CHANNELLEN=" + toString(limits::CHANNELLEN)
		+ " CHANTYPES=#& NICKLEN=" + toString(limits::NICKLEN)
		+ " PREFIX=(o)@ TOPICLEN=" + toString(limits::TOPICLEN)
		+ " USERLEN=" + toString(limits::USERLEN);

	reply(client, RPL_WELCOME, "", "Welcome to the Internet Relay Network " + client.mask());
	reply(client, RPL_YOURHOST, "", "Your host is " + server + ", running version " + version);
	reply(client, RPL_CREATED, "", "This server was created " + createdAt_);
	reply(client, RPL_MYINFO, server + " " + version + " i iklot");
	reply(client, RPL_ISUPPORT, isupport, "are supported by this server");
	reply(client, ERR_NOMOTD, "", "MOTD File is missing");
}
