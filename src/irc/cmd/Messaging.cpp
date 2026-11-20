// PRIVMSG and NOTICE. CTCP and DCC requests are ordinary PRIVMSG text, so
// they pass through unchanged (this is what makes DCC file transfer work).

#include "irc/Core.hpp"

#include "irc/Text.hpp"

void Core::cmdPrivmsg(Client& client, const Message& msg)
{
	relayMessage(client, msg, false);
}

void Core::cmdNotice(Client& client, const Message& msg)
{
	relayMessage(client, msg, true);
}

// NOTICE must never trigger an automatic reply (RFC 2812 3.3.2), so with
// isNotice every error is dropped instead of being sent back.
void Core::relayMessage(Client& client, const Message& msg, bool isNotice)
{
	if (msg.params.empty())
	{
		if (isNotice)
			return;
		throw IrcError(ERR_NORECIPIENT, "", "No recipient given (" + msg.command + ")");
	}
	if (msg.params.size() < 2 || msg.params[1].empty())
	{
		if (isNotice)
			return;
		throw IrcError(ERR_NOTEXTTOSEND, "", "No text to send");
	}
	const std::vector<std::string> targets = splitComma(msg.params[0]);
	for (std::vector<std::string>::size_type i = 0; i < targets.size(); ++i)
	{
		try
		{
			deliverMessage(client, msg.command, targets[i], msg.params[1]);
		}
		catch (const IrcError& error)
		{
			if (!isNotice)
				replyError(client, error);
		}
	}
}

// A channel message goes to every member except the sender, and only
// members may send (the usual +n behaviour, always on here).
void Core::deliverMessage(const Client& sender, const std::string& command,
	const std::string& target, const std::string& text)
{
	if (looksLikeChannel(target))
	{
		const Channel* channel = findChannel(target);
		if (channel == NULL)
			throw IrcError(ERR_NOSUCHNICK, asParam(target), "No such nick/channel");
		if (!channel->isMember(sender.id))
			throw IrcError(ERR_CANNOTSENDTOCHAN, channel->name, "Cannot send to channel");
		broadcastExcept(*channel, ":" + sender.mask() + " " + command + " " + channel->name + " :" + text, sender.id);
		return;
	}
	const Client* recipient = findClientByNick(target);
	if (recipient == NULL)
		throw IrcError(ERR_NOSUCHNICK, asParam(target), "No such nick/channel");
	sendLine(recipient->id, ":" + sender.mask() + " " + command + " " + recipient->nick + " :" + text);
}
