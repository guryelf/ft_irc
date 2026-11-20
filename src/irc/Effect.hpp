#ifndef EFFECT_HPP
#define EFFECT_HPP

#include <string>

// Identifies one connection for its whole life. The shell assigns ids from a
// counter that only goes up, so an id is never reused, unlike an fd.
typedef unsigned long ClientId;

// An instruction from the IRC core to the network shell.
struct Effect
{
	enum Kind
	{
		SEND,	// queue data (one full line ending in "\r\n") for the client
		CLOSE	// flush what is queued, then close; data holds the reason
	};

	Kind kind;
	ClientId id;
	std::string data;

	Effect(Kind effectKind, ClientId target, const std::string& payload)
		: kind(effectKind), id(target), data(payload)
	{
	}
};

#endif
