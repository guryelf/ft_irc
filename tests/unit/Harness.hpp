#ifndef HARNESS_HPP
#define HARNESS_HPP

#include <ctime>
#include <string>
#include <vector>

#include "irc/Core.hpp"

// Drives a Core the way the network shell does, with a fake clock and no sockets.
// Typical use: send(), then flush(), then inspect what was sent or closed.
struct Harness
{
	Core core;
	std::time_t now;
	std::vector<Effect> effects;	// filled by the last flush()

	Harness() : core("secret", 1000), now(1000)
	{
	}

	void connect(ClientId id)
	{
		core.onConnect(id, "127.0.0.1", now);
	}

	// Connects and fully registers id; the user name equals the nick.
	// The welcome burst is flushed away.
	void registerClient(ClientId id, const std::string& nick)
	{
		connect(id);
		send(id, "PASS secret\r\nNICK " + nick + "\r\nUSER " + nick + " 0 * :Real Name\r\n");
		flush();
	}

	void send(ClientId id, const std::string& bytes)
	{
		core.onData(id, bytes, now);
	}

	// Advances the clock, then runs one tick.
	void tick(std::time_t seconds)
	{
		now += seconds;
		core.onTick(now);
	}

	void flush()
	{
		core.takeEffects(effects);
	}

	// Lines sent to id by the last flush, without CRLF.
	std::vector<std::string> sentTo(ClientId id) const
	{
		std::vector<std::string> lines;
		for (std::vector<Effect>::size_type i = 0; i < effects.size(); ++i)
		{
			const Effect& effect = effects[i];
			if (effect.kind == Effect::SEND && effect.id == id)
				lines.push_back(effect.data.substr(0, effect.data.size() - 2));
		}
		return lines;
	}

	// Same lines joined by '\n': one string compares well in CHECK_EQ.
	std::string joinedTo(ClientId id) const
	{
		const std::vector<std::string> lines = sentTo(id);
		std::string joined;
		for (std::vector<std::string>::size_type i = 0; i < lines.size(); ++i)
		{
			if (i > 0)
				joined += '\n';
			joined += lines[i];
		}
		return joined;
	}

	// The command or numeric (second word) of each line sent to id:
	// "001 002 003" reads better in a check than whole lines.
	std::string codesTo(ClientId id) const
	{
		const std::vector<std::string> lines = sentTo(id);
		std::string codes;
		for (std::vector<std::string>::size_type i = 0; i < lines.size(); ++i)
		{
			const std::string::size_type start = lines[i].find(' ') + 1;
			if (i > 0)
				codes += ' ';
			codes += lines[i].substr(start, lines[i].find(' ', start) - start);
		}
		return codes;
	}

	bool closed(ClientId id) const
	{
		for (std::vector<Effect>::size_type i = 0; i < effects.size(); ++i)
		{
			if (effects[i].kind == Effect::CLOSE && effects[i].id == id)
				return true;
		}
		return false;
	}
};

#endif
