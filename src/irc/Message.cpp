#include "irc/Message.hpp"

#include "Limits.hpp"
#include "irc/Text.hpp"

namespace
{

// RFC 2812 separates tokens with SPACE (0x20) only; repeated spaces are tolerated.
void skipSpaces(const std::string& line, std::string::size_type& pos)
{
	while (pos < line.size() && line[pos] == ' ')
		++pos;
}

// Returns the token starting at pos and moves pos to the space after it.
std::string nextToken(const std::string& line, std::string::size_type& pos)
{
	const std::string::size_type end = line.find(' ', pos);
	const std::string::size_type stop = (end == std::string::npos) ? line.size() : end;
	const std::string token = line.substr(pos, stop - pos);
	pos = stop;
	return token;
}

}

bool Message::parse(const std::string& line, Message& out)
{
	std::string::size_type pos = 0;

	out = Message();
	skipSpaces(line, pos);
	if (pos < line.size() && line[pos] == ':')
	{
		++pos;
		out.prefix = nextToken(line, pos);
		skipSpaces(line, pos);
	}
	out.command = asciiUpper(nextToken(line, pos));
	if (out.command.empty())
		return false;
	for (;;)
	{
		skipSpaces(line, pos);
		if (pos >= line.size())
			break;
		if (line[pos] == ':' || out.params.size() == limits::MAX_PARAMS - 1)
		{
			if (line[pos] == ':')
				++pos;
			out.params.push_back(line.substr(pos));
			break;
		}
		out.params.push_back(nextToken(line, pos));
	}
	return true;
}
