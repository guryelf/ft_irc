#include "irc/Channel.hpp"

#include "irc/Text.hpp"

Channel::Channel(const std::string& channelName)
	: name(channelName),
	  topicSetAt(0),
	  limit(0),
	  inviteOnly(false),
	  topicRestricted(false)
{
}

void Channel::addMember(ClientId id, bool asOperator)
{
	members_.insert(id);
	if (asOperator)
		operators_.insert(id);
	invited_.erase(id);
}

void Channel::removeMember(ClientId id)
{
	members_.erase(id);
	operators_.erase(id);
}

void Channel::setOperator(ClientId id, bool op)
{
	if (!isMember(id))
		return;
	if (op)
		operators_.insert(id);
	else
		operators_.erase(id);
}

void Channel::invite(ClientId id)
{
	invited_.insert(id);
}

void Channel::uninvite(ClientId id)
{
	invited_.erase(id);
}

bool Channel::isMember(ClientId id) const
{
	return members_.count(id) != 0;
}

bool Channel::isOperator(ClientId id) const
{
	return operators_.count(id) != 0;
}

bool Channel::isInvited(ClientId id) const
{
	return invited_.count(id) != 0;
}

bool Channel::empty() const
{
	return members_.empty();
}

std::size_t Channel::size() const
{
	return members_.size();
}

const std::set<ClientId>& Channel::members() const
{
	return members_;
}

std::string Channel::modes(bool withParams) const
{
	std::string flags = "+";
	std::string params;

	if (inviteOnly)
		flags += 'i';
	if (topicRestricted)
		flags += 't';
	if (!key.empty())
	{
		flags += 'k';
		if (withParams)
			params += " " + key;
	}
	if (limit > 0)
	{
		flags += 'l';
		if (withParams)
			params += " " + toString(limit);
	}
	return flags + params;
}
