#ifndef CHANNEL_HPP
#define CHANNEL_HPP

#include <cstddef>
#include <ctime>
#include <set>
#include <string>

#include "irc/Effect.hpp"

// One channel. Settings are public fields because no rule ties them together.
// Membership is private: operators and invites must stay consistent with the
// member set, so only these methods change it.
class Channel
{
public:
	explicit Channel(const std::string& channelName);

	std::string name;			// spelling used at creation, for display
	std::string topic;
	std::string topicSetter;	// nick that set the topic
	std::time_t topicSetAt;
	std::string key;			// +k, empty when unset
	std::size_t limit;			// +l, 0 when unset
	bool inviteOnly;			// +i
	bool topicRestricted;		// +t

	void addMember(ClientId id, bool asOperator);	// also consumes a pending invite
	void removeMember(ClientId id);					// also drops operator status
	void setOperator(ClientId id, bool op);			// ignored for non-members
	void invite(ClientId id);
	void uninvite(ClientId id);

	bool isMember(ClientId id) const;
	bool isOperator(ClientId id) const;
	bool isInvited(ClientId id) const;
	bool empty() const;
	std::size_t size() const;
	const std::set<ClientId>& members() const;

	// "+itkl", followed by the key and the limit when withParams is true.
	std::string modes(bool withParams) const;

private:
	std::set<ClientId> members_;
	std::set<ClientId> operators_;	// always a subset of members_
	std::set<ClientId> invited_;
};

#endif
