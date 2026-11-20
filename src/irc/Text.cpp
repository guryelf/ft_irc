#include "irc/Text.hpp"

#include <sstream>

#include "Limits.hpp"

namespace
{

// ASCII checks written out by hand: <cctype> depends on the locale and is
// undefined for negative char values.
bool isLetter(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool isDigit(char c)
{
	return c >= '0' && c <= '9';
}

bool isSpecial(char c)
{
	return std::string("[]\\`_^{|}").find(c) != std::string::npos;
}

}

std::string ircLower(const std::string& text)
{
	std::string folded(text);

	for (std::string::size_type i = 0; i < folded.size(); ++i)
	{
		const char c = folded[i];
		if (c >= 'A' && c <= 'Z')
			folded[i] = static_cast<char>(c - 'A' + 'a');
		else if (c == '[')
			folded[i] = '{';
		else if (c == ']')
			folded[i] = '}';
		else if (c == '\\')
			folded[i] = '|';
		else if (c == '~')
			folded[i] = '^';
	}
	return folded;
}

bool isValidNick(const std::string& nick)
{
	if (nick.empty() || nick.size() > limits::NICKLEN)
		return false;
	if (!isLetter(nick[0]) && !isSpecial(nick[0]))
		return false;
	for (std::string::size_type i = 1; i < nick.size(); ++i)
	{
		const char c = nick[i];
		if (!isLetter(c) && !isDigit(c) && !isSpecial(c) && c != '-')
			return false;
	}
	return true;
}

bool isValidChannelName(const std::string& name)
{
	if (name.size() < 2 || name.size() > limits::CHANNELLEN)
		return false;
	if (name[0] != '#' && name[0] != '&')
		return false;
	for (std::string::size_type i = 1; i < name.size(); ++i)
	{
		const char c = name[i];
		if (c == ' ' || c == ',' || c == ':' || c == '\0' || c == '\a' || c == '\r' || c == '\n')
			return false;
	}
	return true;
}

bool looksLikeChannel(const std::string& target)
{
	return !target.empty() && (target[0] == '#' || target[0] == '&');
}

std::vector<std::string> splitComma(const std::string& list)
{
	std::vector<std::string> items;

	if (list.empty())
		return items;
	std::string::size_type start = 0;
	for (;;)
	{
		const std::string::size_type comma = list.find(',', start);
		if (comma == std::string::npos)
		{
			items.push_back(list.substr(start));
			return items;
		}
		items.push_back(list.substr(start, comma - start));
		start = comma + 1;
	}
}

std::string toString(unsigned long value)
{
	std::ostringstream out;
	out << value;
	return out.str();
}

std::string asciiUpper(const std::string& text)
{
	std::string upper(text);

	for (std::string::size_type i = 0; i < upper.size(); ++i)
	{
		if (upper[i] >= 'a' && upper[i] <= 'z')
			upper[i] = static_cast<char>(upper[i] - 'a' + 'A');
	}
	return upper;
}

std::string asParam(const std::string& text)
{
	if (text.empty() || text[0] == ':' || text.find(' ') != std::string::npos)
		return "*";
	return text;
}
