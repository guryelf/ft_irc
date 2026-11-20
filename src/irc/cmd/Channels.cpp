// Channel membership and information: JOIN, PART, TOPIC, NAMES, WHO.

#include "irc/Core.hpp"

#include "Limits.hpp"
#include "irc/Text.hpp"

void Core::cmdJoin(Client& client, const Message& msg)
{
	if (msg.params[0] == "0")
	{
		// "JOIN 0" leaves every channel (RFC 2812 3.2.1). Iterate over a
		// copy: leaving edits client.channels.
		const std::set<std::string> joined = client.channels;
		for (std::set<std::string>::const_iterator key = joined.begin(); key != joined.end(); ++key)
		{
			const ChannelMap::const_iterator channel = channels_.find(*key);
			if (channel != channels_.end())
				broadcast(channel->second, ":" + client.mask() + " PART " + channel->second.name);
			leaveChannel(client, *key);
		}
		return;
	}
	// "JOIN #a,#b key1,key2": keys match channels by position. Each channel
	// succeeds or fails on its own.
	const std::vector<std::string> names = splitComma(msg.params[0]);
	const std::vector<std::string> keys = (msg.params.size() > 1) ? splitComma(msg.params[1]) : std::vector<std::string>();
	for (std::vector<std::string>::size_type i = 0; i < names.size(); ++i)
	{
		try
		{
			joinChannel(client, names[i], (i < keys.size()) ? keys[i] : "");
		}
		catch (const IrcError& error)
		{
			replyError(client, error);
		}
	}
}

// All checks run before anything changes, so a refused JOIN leaves no trace.
// INVITE lifts +i only; +k and +l apply to invited clients too.
void Core::joinChannel(Client& client, const std::string& name, const std::string& key)
{
	if (!isValidChannelName(name))
		throw IrcError(ERR_BADCHANMASK, asParam(name), "Bad Channel Mask");
	const std::string folded = ircLower(name);
	if (client.channels.count(folded) != 0)
		return;
	if (client.channels.size() >= limits::CHANLIMIT)
		throw IrcError(ERR_TOOMANYCHANNELS, name, "You have joined too many channels");

	ChannelMap::iterator it = channels_.find(folded);
	const bool creating = (it == channels_.end());
	if (!creating)
	{
		const Channel& existing = it->second;
		if (existing.inviteOnly && !existing.isInvited(client.id))
			throw IrcError(ERR_INVITEONLYCHAN, existing.name, "Cannot join channel (+i)");
		if (!existing.key.empty() && key != existing.key)
			throw IrcError(ERR_BADCHANNELKEY, existing.name, "Cannot join channel (+k)");
		if (existing.limit > 0 && existing.size() >= existing.limit)
			throw IrcError(ERR_CHANNELISFULL, existing.name, "Cannot join channel (+l)");
	}
	else
		it = channels_.insert(std::make_pair(folded, Channel(name))).first;

	Channel& channel = it->second;
	channel.addMember(client.id, creating);	// the creator becomes operator
	client.channels.insert(folded);
	broadcast(channel, ":" + client.mask() + " JOIN " + channel.name);
	if (!channel.topic.empty())
		sendTopic(client, channel);
	sendNames(client, channel);
}

void Core::cmdPart(Client& client, const Message& msg)
{
	const std::vector<std::string> names = splitComma(msg.params[0]);
	const std::string reason = (msg.params.size() > 1) ? msg.params[1] : "";

	for (std::vector<std::string>::size_type i = 0; i < names.size(); ++i)
	{
		try
		{
			const Channel& channel = requireChannel(names[i]);
			requireMember(channel, client);
			std::string line = ":" + client.mask() + " PART " + channel.name;
			if (!reason.empty())
				line += " :" + reason;
			broadcast(channel, line);
			// May delete the channel: do not use it after this.
			leaveChannel(client, ircLower(names[i]));
		}
		catch (const IrcError& error)
		{
			replyError(client, error);
		}
	}
}

void Core::cmdTopic(Client& client, const Message& msg)
{
	Channel& channel = requireChannel(msg.params[0]);
	requireMember(channel, client);

	if (msg.params.size() == 1)
	{
		if (channel.topic.empty())
			reply(client, RPL_NOTOPIC, channel.name, "No topic is set");
		else
			sendTopic(client, channel);
		return;
	}
	if (channel.topicRestricted)
		requireOperator(channel, client);
	// An empty trailing parameter ("TOPIC #c :") clears the topic.
	channel.topic = msg.params[1].substr(0, limits::TOPICLEN);
	channel.topicSetter = client.nick;
	channel.topicSetAt = now_;
	broadcast(channel, ":" + client.mask() + " TOPIC " + channel.name + " :" + channel.topic);
}

void Core::cmdNames(Client& client, const Message& msg)
{
	if (msg.params.empty())
	{
		reply(client, RPL_ENDOFNAMES, "*", "End of /NAMES list");
		return;
	}
	const std::vector<std::string> names = splitComma(msg.params[0]);
	for (std::vector<std::string>::size_type i = 0; i < names.size(); ++i)
	{
		const Channel* channel = findChannel(names[i]);
		if (channel != NULL)
			sendNames(client, *channel);
		else
			reply(client, RPL_ENDOFNAMES, asParam(names[i]), "End of /NAMES list");
	}
}

// Minimal WHO: a channel name lists its members, a nick lists that client.
// Clients such as irssi send "WHO #channel" after every JOIN.
void Core::cmdWho(Client& client, const Message& msg)
{
	const std::string mask = msg.params.empty() ? "*" : msg.params[0];

	if (looksLikeChannel(mask))
	{
		const Channel* channel = findChannel(mask);
		if (channel != NULL)
		{
			const std::set<ClientId>& members = channel->members();
			for (std::set<ClientId>::const_iterator id = members.begin(); id != members.end(); ++id)
			{
				const Client* member = findClient(*id);
				if (member != NULL)
					sendWhoReply(client, channel->name, *member, channel->isOperator(*id));
			}
		}
	}
	else
	{
		const Client* target = findClientByNick(mask);
		if (target != NULL)
			sendWhoReply(client, "*", *target, false);
	}
	reply(client, RPL_ENDOFWHO, asParam(mask), "End of WHO list");
}

// "<channel> <user> <host> <server> <nick> H[@] :0 <realname>": H means
// here (not away), @ marks an operator, 0 is the hop count.
void Core::sendWhoReply(const Client& client, const std::string& channelName, const Client& member, bool isOperator)
{
	reply(client, RPL_WHOREPLY,
		channelName + " " + member.user + " " + member.host + " " + limits::SERVER_NAME + " "
			+ member.nick + " H" + (isOperator ? "@" : ""),
		"0 " + member.realname);
}
