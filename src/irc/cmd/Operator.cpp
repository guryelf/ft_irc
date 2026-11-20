// Channel operator commands: KICK, INVITE and MODE (channel and user modes).

#include "irc/Core.hpp"

#include "irc/Text.hpp"

namespace
{

// Collects the changes MODE really applied, to announce them in one line
// such as "+ik-o key bob". A sign is written only when it differs from the
// previous one.
class ModeChanges
{
public:
	ModeChanges() : sign_('\0')
	{
	}

	void add(bool adding, char mode)
	{
		const char sign = adding ? '+' : '-';
		if (sign != sign_)
			flags_ += sign;
		sign_ = sign;
		flags_ += mode;
	}

	void add(bool adding, char mode, const std::string& param)
	{
		add(adding, mode);
		params_ += " " + param;
	}

	bool empty() const
	{
		return flags_.empty();
	}

	std::string str() const
	{
		return flags_ + params_;
	}

private:
	std::string flags_;
	std::string params_;
	char sign_;
};

// A key must survive "JOIN #a,#b key1,key2": no comma, no space, and no
// leading ':' (which would turn it into a trailing parameter).
bool isValidKey(const std::string& key)
{
	return !key.empty() && key[0] != ':' && key.find_first_of(", ") == std::string::npos;
}

// A user limit is a positive decimal number of at most 9 digits.
bool parseLimit(const std::string& text, std::size_t& limit)
{
	if (text.empty() || text.size() > 9)
		return false;
	std::size_t value = 0;
	for (std::string::size_type i = 0; i < text.size(); ++i)
	{
		if (text[i] < '0' || text[i] > '9')
			return false;
		value = value * 10 + static_cast<std::size_t>(text[i] - '0');
	}
	if (value == 0)
		return false;
	limit = value;
	return true;
}

}

// ---------------------------------------------------------------- KICK

void Core::cmdKick(Client& client, const Message& msg)
{
	const Channel& channel = requireChannel(msg.params[0]);
	requireMember(channel, client);
	requireOperator(channel, client);

	const std::string channelName = channel.name;
	const std::string channelKey = ircLower(channelName);
	const std::string comment = (msg.params.size() > 2 && !msg.params[2].empty()) ? msg.params[2] : client.nick;
	const std::vector<std::string> targets = splitComma(msg.params[1]);
	for (std::vector<std::string>::size_type i = 0; i < targets.size(); ++i)
	{
		// An operator who kicks themselves loses the right to continue, and
		// may have emptied (deleted) the channel: look it up every time.
		Channel* current = findChannel(channelKey);
		if (current == NULL || !current->isOperator(client.id))
			break;
		try
		{
			Client* victim = findClientByNick(targets[i]);
			if (victim == NULL)
				throw IrcError(ERR_NOSUCHNICK, asParam(targets[i]), "No such nick/channel");
			if (!current->isMember(victim->id))
				throw IrcError(ERR_USERNOTINCHANNEL, victim->nick + " " + channelName, "They aren't on that channel");
			// The victim sees the KICK too: it is still a member when this is sent.
			broadcast(*current, ":" + client.mask() + " KICK " + channelName + " " + victim->nick + " :" + comment);
			leaveChannel(*victim, channelKey);
		}
		catch (const IrcError& error)
		{
			replyError(client, error);
		}
	}
}

// ---------------------------------------------------------------- INVITE

// Any member may invite to a normal channel; only operators to a +i channel.
// The invite lets the target past +i once (JOIN consumes it).
void Core::cmdInvite(Client& client, const Message& msg)
{
	const Client* target = findClientByNick(msg.params[0]);
	if (target == NULL)
		throw IrcError(ERR_NOSUCHNICK, asParam(msg.params[0]), "No such nick/channel");
	Channel& channel = requireChannel(msg.params[1]);
	requireMember(channel, client);
	if (channel.inviteOnly)
		requireOperator(channel, client);
	if (channel.isMember(target->id))
		throw IrcError(ERR_USERONCHANNEL, target->nick + " " + channel.name, "is already on channel");

	channel.invite(target->id);
	reply(client, RPL_INVITING, target->nick + " " + channel.name);
	sendLine(target->id, ":" + client.mask() + " INVITE " + target->nick + " " + channel.name);
}

// ---------------------------------------------------------------- MODE

void Core::cmdMode(Client& client, const Message& msg)
{
	if (looksLikeChannel(msg.params[0]))
		channelMode(client, msg);
	else
		userMode(client, msg);
}

// "MODE #c" shows the modes (the key only to members). "MODE #c b" asks for
// the ban list, which irssi does on every join; bans do not exist here, so
// the list is always empty. Any change requires channel operator status.
//
// Modes are applied left to right; each one is checked on its own, so one
// bad mode is reported without cancelling the others. Parameters are taken
// in order: +k <key>, -k [key], +l <limit>, +o/-o <nick>.
void Core::channelMode(Client& client, const Message& msg)
{
	Channel& channel = requireChannel(msg.params[0]);

	if (msg.params.size() == 1)
	{
		reply(client, RPL_CHANNELMODEIS, channel.name + " " + channel.modes(channel.isMember(client.id)));
		return;
	}
	const std::string& modestring = msg.params[1];
	if (msg.params.size() == 2 && (modestring == "b" || modestring == "+b"))
	{
		reply(client, RPL_ENDOFBANLIST, channel.name, "End of channel ban list");
		return;
	}
	requireMember(channel, client);
	requireOperator(channel, client);

	ModeChanges changes;
	bool adding = true;
	std::vector<std::string>::size_type next = 2;	// next unused parameter
	for (std::string::size_type i = 0; i < modestring.size(); ++i)
	{
		const char mode = modestring[i];
		if (mode == '+' || mode == '-')
		{
			adding = (mode == '+');
			continue;
		}
		try
		{
			if (mode == 'i' || mode == 't')
			{
				bool& flag = (mode == 'i') ? channel.inviteOnly : channel.topicRestricted;
				if (flag != adding)
				{
					flag = adding;
					changes.add(adding, mode);
				}
			}
			else if (mode == 'k' && adding)
			{
				if (next >= msg.params.size())
					throw IrcError(ERR_NEEDMOREPARAMS, "MODE", "Not enough parameters");
				const std::string& key = msg.params[next++];
				if (!isValidKey(key))
					throw IrcError(ERR_INVALIDKEY, channel.name, "Key is not well-formed");
				if (channel.key != key)
				{
					channel.key = key;
					changes.add(true, 'k', key);
				}
			}
			else if (mode == 'k')
			{
				// "-k key" is the usual form; the key value itself is not checked.
				if (next < msg.params.size())
					++next;
				if (!channel.key.empty())
				{
					channel.key.clear();
					// k always takes a parameter (CHANMODES type B), so clients
					// expect one here too.
					changes.add(false, 'k', "*");
				}
			}
			else if (mode == 'l' && adding)
			{
				if (next >= msg.params.size())
					throw IrcError(ERR_NEEDMOREPARAMS, "MODE", "Not enough parameters");
				std::size_t limit = 0;
				// An invalid limit ("abc", "0") is ignored, as most servers do.
				if (parseLimit(msg.params[next++], limit) && channel.limit != limit)
				{
					channel.limit = limit;
					changes.add(true, 'l', toString(limit));
				}
			}
			else if (mode == 'l')
			{
				if (channel.limit != 0)
				{
					channel.limit = 0;
					changes.add(false, 'l');
				}
			}
			else if (mode == 'o')
			{
				if (next >= msg.params.size())
					throw IrcError(ERR_NEEDMOREPARAMS, "MODE", "Not enough parameters");
				const std::string& nick = msg.params[next++];
				const Client* target = findClientByNick(nick);
				if (target == NULL)
					throw IrcError(ERR_NOSUCHNICK, asParam(nick), "No such nick/channel");
				if (!channel.isMember(target->id))
					throw IrcError(ERR_USERNOTINCHANNEL, target->nick + " " + channel.name, "They aren't on that channel");
				if (channel.isOperator(target->id) != adding)
				{
					channel.setOperator(target->id, adding);
					changes.add(adding, 'o', target->nick);
				}
			}
			else
				throw IrcError(ERR_UNKNOWNMODE, asParam(std::string(1, mode)), "is unknown mode char to me");
		}
		catch (const IrcError& error)
		{
			replyError(client, error);
		}
	}
	if (!changes.empty())
		broadcast(channel, ":" + client.mask() + " MODE " + channel.name + " " + changes.str());
}

// Only the client's own modes, and only +i (which irssi sets right after
// connecting). It is stored and reported but changes nothing else.
void Core::userMode(Client& client, const Message& msg)
{
	const std::string& target = msg.params[0];

	if (ircLower(target) != ircLower(client.nick))
	{
		if (findClientByNick(target) == NULL)
			throw IrcError(ERR_NOSUCHNICK, asParam(target), "No such nick/channel");
		throw IrcError(ERR_USERSDONTMATCH, "", "Cant change mode for other users");
	}
	if (msg.params.size() == 1)
	{
		reply(client, RPL_UMODEIS, client.invisible ? "+i" : "+");
		return;
	}
	const std::string& modestring = msg.params[1];
	ModeChanges changes;
	bool adding = true;
	bool unknown = false;
	for (std::string::size_type i = 0; i < modestring.size(); ++i)
	{
		const char mode = modestring[i];
		if (mode == '+' || mode == '-')
			adding = (mode == '+');
		else if (mode == 'i')
		{
			if (client.invisible != adding)
			{
				client.invisible = adding;
				changes.add(adding, 'i');
			}
		}
		else
			unknown = true;
	}
	if (!changes.empty())
		sendLine(client.id, ":" + client.mask() + " MODE " + client.nick + " :" + changes.str());
	if (unknown)
		throw IrcError(ERR_UMODEUNKNOWNFLAG, "", "Unknown MODE flag");
}
