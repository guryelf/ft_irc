#ifndef MESSAGE_HPP
#define MESSAGE_HPP

#include <string>
#include <vector>

// One parsed IRC message: [":" prefix " "] command [params].
struct Message
{
	std::string prefix;
	std::string command;				// always upper case
	std::vector<std::string> params;	// at most limits::MAX_PARAMS

	// Parses one line whose line ending is already removed (RFC 2812 2.3.1).
	// A parameter starting with ':' is the trailing one and takes the rest of
	// the line, spaces included; so does the 15th parameter.
	// Returns false when the line has no command.
	static bool parse(const std::string& line, Message& out);
};

#endif
