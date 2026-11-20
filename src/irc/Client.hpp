#ifndef CLIENT_HPP
#define CLIENT_HPP

#include <ctime>
#include <set>
#include <string>

#include "irc/Effect.hpp"

// The core's record of one connection. It is a plain struct because every
// rule that spans clients (unique nicks, channel membership) is enforced by
// Core, which owns all Client objects.
struct Client
{
	Client(ClientId clientId, const std::string& clientHost, std::time_t now);

	// "nick!user@host": the prefix of every message this client originates.
	std::string mask() const;
	// The nick, or "*" before one is set: numeric replies always need a target.
	std::string displayNick() const;

	ClientId id;
	std::string host;

	// Identity
	std::string nick;
	std::string user;
	std::string realname;

	// Registration
	bool passOk;
	bool capNegotiating;	// between "CAP LS" and "CAP END"
	bool registered;
	bool invisible;			// user mode +i: stored and reported, no other effect

	// Input framing and flood control
	std::string inbuf;		// received bytes not yet consumed as lines
	bool discarding;		// dropping the rest of an oversized line
	bool throttled;			// a complete line waits for flood tokens
	unsigned floodTokens;
	std::time_t floodRefilledAt;

	// Liveness
	std::time_t connectedAt;
	std::time_t lastActivity;
	bool pingPending;
	std::time_t pingSentAt;

	// Joined channels, as ircLower(name): the keys of Core's channel map
	std::set<std::string> channels;
};

#endif
