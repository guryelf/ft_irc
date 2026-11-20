#include "irc/Client.hpp"

#include "Limits.hpp"

Client::Client(ClientId clientId, const std::string& clientHost, std::time_t now)
	: id(clientId),
	  host(clientHost),
	  passOk(false),
	  capNegotiating(false),
	  registered(false),
	  invisible(false),
	  discarding(false),
	  throttled(false),
	  floodTokens(limits::FLOOD_BURST),
	  floodRefilledAt(now),
	  connectedAt(now),
	  lastActivity(now),
	  pingPending(false),
	  pingSentAt(0)
{
}

std::string Client::mask() const
{
	return nick + "!" + user + "@" + host;
}

std::string Client::displayNick() const
{
	return nick.empty() ? "*" : nick;
}
