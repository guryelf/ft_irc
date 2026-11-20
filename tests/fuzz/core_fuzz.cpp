// Grammar-based random testing of the IRC core, without sockets.
//
// Many simulated clients send random but IRC-shaped traffic: valid and
// broken registrations, every command with hostile parameters, random packet
// splits, CR injection, oversized lines, random disconnects, clock jumps that
// trigger timeouts, and server shutdowns. Every Effect the core produces is
// checked:
//   - a SEND is exactly one line: ends in CRLF, at most 512 bytes, no CR or
//     LF inside, and parses as an IRC message from the server or a client
//   - nothing is sent to, or closed for, a client after it was closed
//   - a closed client never asks for input again
//   - no exception escapes the core
//
// Usage: core_fuzz [iterations] [seed]
// Build with sanitizers (make fuzz) so memory errors are caught as well.

#include <cstdlib>
#include <exception>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "irc/Core.hpp"
#include "irc/Message.hpp"

namespace
{

// xorshift32: small, fast, and reproducible from the seed.
class Random
{
public:
	explicit Random(unsigned seed) : state_(seed == 0 ? 2463534242u : seed)
	{
	}

	unsigned next()
	{
		state_ ^= state_ << 13;
		state_ ^= state_ >> 17;
		state_ ^= state_ << 5;
		return state_;
	}

	unsigned below(unsigned bound)
	{
		return next() % bound;
	}

	bool chance(unsigned percent)
	{
		return below(100) < percent;
	}

	const std::string& pick(const std::vector<std::string>& items)
	{
		return items[below(static_cast<unsigned>(items.size()))];
	}

private:
	unsigned state_;
};

std::vector<std::string> makeList(const char* const* items)
{
	std::vector<std::string> list;
	for (; *items != NULL; ++items)
		list.push_back(*items);
	return list;
}

const char* const NICKS[] = {
	"n0", "n1", "n2", "n3", "n4", "n5", "n6", "n7", "N1", "[x]", "{X}", "a-b", "`q",
	"1bad", "-bad", "*", "#notnick", "n0,n1", "nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn", NULL
};
const char* const CHANNELS[] = {
	"#a", "#A", "#b", "#c", "&loc", "#", "a", "#a,#b", "#a,,#c", "#bad:x", "#\a",
	"#cccccccccccccccccccccccccccccccccccccccccccccccccc", "0", "#B", NULL
};
const char* const COMMANDS[] = {
	"PASS", "NICK", "USER", "CAP", "PING", "PONG", "QUIT", "JOIN", "PART", "TOPIC",
	"NAMES", "WHO", "PRIVMSG", "NOTICE", "KICK", "INVITE", "MODE", "privmsg", "join",
	"mode", "FOO", "001", ":pfx", "", NULL
};

class Fuzzer
{
public:
	Fuzzer(unsigned seed)
		: random_(seed), core_("secret", 1000), now_(1000), nextId_(1),
		  nicks_(makeList(NICKS)), channels_(makeList(CHANNELS)), commands_(makeList(COMMANDS)),
		  linesFed_(0), effectsChecked_(0), closes_(0), failed_(false)
	{
	}

	bool run(unsigned long iterations)
	{
		try
		{
			for (iteration_ = 0; iteration_ < iterations && !failed_; ++iteration_)
				step();
			if (!failed_)
			{
				action_ = "final shutdown";
				core_.onShutdown();
				checkEffects();
			}
		}
		catch (const std::exception& e)
		{
			fail(std::string("exception escaped the core: ") + e.what());
		}
		return !failed_;
	}

	void report() const
	{
		std::cout << "iterations " << iteration_ << ", clients " << (nextId_ - 1)
			<< ", lines fed " << linesFed_ << ", effects checked " << effectsChecked_
			<< ", closes " << closes_ << "\nreplies seen (" << seen_.size() << "):";
		for (std::map<std::string, unsigned long>::const_iterator it = seen_.begin(); it != seen_.end(); ++it)
			std::cout << " " << it->first;
		std::cout << std::endl;
	}

private:
	void step()
	{
		const unsigned roll = random_.below(100);

		if (live_.size() < 4 || (roll < 6 && live_.size() < 60))
			connect();
		else if (roll < 9)
			closeRandom();
		else if (roll < 12)
			tick();
		else if (roll == 12 && random_.chance(2))
		{
			action_ = "shutdown";
			core_.onShutdown();
			checkEffects();
		}
		else
			sendTraffic(randomLive());
		checkDeadStayDead();
	}

	void connect()
	{
		const ClientId id = nextId_++;
		action_ = "connect";
		core_.onConnect(id, random_.chance(20) ? "0::1" : "127.0.0.1", now_);
		live_.insert(id);
		checkEffects();
		if (random_.chance(60))
		{
			// Mostly valid registrations, so deeper commands get exercised.
			std::string nick = "n" + std::string(1, static_cast<char>('0' + random_.below(8)));
			std::string pass = random_.chance(90) ? "secret" : "wrong";
			std::string text;
			if (random_.chance(20))
				text += "CAP LS 302\r\n";
			text += "PASS " + pass + "\r\nNICK " + nick + "\r\nUSER u" + nick + " 0 * :Real " + nick + "\r\n";
			if (text.find("CAP") != std::string::npos)
				text += "CAP END\r\n";
			feed(id, text);
		}
	}

	void closeRandom()
	{
		const ClientId id = randomLive();
		action_ = "shell close";
		core_.onClose(id, "Connection closed");
		markDead(id);
		checkEffects();
	}

	void tick()
	{
		action_ = "tick";
		if (random_.chance(10))
			now_ += 100 + random_.below(300);		// long silence: pings, timeouts
		else
			now_ += random_.below(3);
		core_.onTick(now_);
		checkEffects();
	}

	void sendTraffic(ClientId id)
	{
		std::string bytes;
		const unsigned lines = 1 + random_.below(4);
		for (unsigned i = 0; i < lines; ++i)
		{
			bytes += randomLine();
			const unsigned ending = random_.below(100);
			bytes += (ending < 70) ? "\r\n" : (ending < 90) ? "\n" : (ending < 95) ? "\r" : "";
		}
		if (random_.chance(3))
			bytes += std::string(400 + random_.below(5000), 'Z');	// oversized, maybe unterminated
		feed(id, bytes);
	}

	// Splits the bytes at random points, like TCP segments arriving apart.
	void feed(ClientId id, const std::string& bytes)
	{
		std::string::size_type pos = 0;
		while (pos < bytes.size() && live_.count(id) != 0)
		{
			const std::string::size_type size = random_.chance(50) ? bytes.size() - pos : 1 + random_.below(40);
			const std::string chunk = bytes.substr(pos, size);
			pos += chunk.size();
			action_ = "data: " + escape(chunk);
			core_.onData(id, chunk, now_);
			++linesFed_;
			checkEffects();
		}
	}

	std::string randomLine()
	{
		if (random_.chance(25))
		{
			// Unstructured: random command and random tokens.
			std::string line = random_.pick(commands_);
			const unsigned params = random_.below(6);
			for (unsigned i = 0; i < params; ++i)
				line += " " + randomToken();
			if (random_.chance(50))
				line += " :" + randomText();
			return line;
		}
		const std::string nick = random_.pick(nicks_);
		const std::string channel = random_.pick(channels_);
		switch (random_.below(16))
		{
			case 0: return "NICK " + nick;
			case 1: return "JOIN " + channel + (random_.chance(30) ? " " + randomToken() : "");
			case 2: return "PART " + channel + (random_.chance(50) ? " :" + randomText() : "");
			case 3: return "PRIVMSG " + (random_.chance(50) ? channel : nick) + " :" + randomText();
			case 4: return "NOTICE " + (random_.chance(50) ? channel : nick) + " :" + randomText();
			case 5: return "TOPIC " + channel + (random_.chance(60) ? " :" + randomText() : "");
			case 6: return "KICK " + channel + " " + nick + (random_.chance(50) ? " :" + randomText() : "");
			case 7: return "INVITE " + nick + " " + channel;
			case 8: return "MODE " + channel + (random_.chance(15) ? "" : " " + randomModes() + randomModeParams());
			case 9: return "MODE " + nick + (random_.chance(70) ? " " + randomModes() : "");
			case 10: return "NAMES " + channel;
			case 11: return "WHO " + (random_.chance(50) ? channel : nick);
			case 12: return random_.chance(10) ? "QUIT :" + randomText() : "PING :" + randomToken();
			case 13: return "CAP " + std::string(random_.chance(50) ? "LS 302" : random_.chance(50) ? "END" : "REQ :x");
			case 14: return ":" + nick + "!spoof@host PRIVMSG " + channel + " :" + randomText();
			default: return "JOIN 0";
		}
	}

	std::string randomModes()
	{
		static const char letters[] = "+-itkolbxz";
		std::string modes;
		const unsigned length = 1 + random_.below(8);
		for (unsigned i = 0; i < length; ++i)
			modes += letters[random_.below(sizeof(letters) - 1)];
		return modes;
	}

	std::string randomModeParams()
	{
		std::string params;
		const unsigned count = random_.below(5);
		for (unsigned i = 0; i < count; ++i)
		{
			const unsigned kind = random_.below(5);
			params += " " + (kind == 0 ? random_.pick(nicks_) : kind == 1 ? std::string("2") : kind == 2 ? std::string("0")
				: kind == 3 ? std::string("1") : randomToken());
		}
		return params;
	}

	std::string randomToken()
	{
		static const char alphabet[] = "abcXYZ019#&,:*!@[]{}\\|^`_-+";
		std::string token;
		const unsigned length = 1 + random_.below(12);
		for (unsigned i = 0; i < length; ++i)
			token += alphabet[random_.below(sizeof(alphabet) - 1)];
		return token;
	}

	// Free text: any byte except LF, including NUL, CTCP \001, high bytes,
	// and the occasional CR (which must split the line, never travel inside).
	std::string randomText()
	{
		std::string text;
		const unsigned length = random_.chance(5) ? 300 + random_.below(400) : random_.below(40);
		for (unsigned i = 0; i < length; ++i)
		{
			char c = static_cast<char>(random_.below(256));
			if (c == '\n' || (c == '\r' && !random_.chance(5)))
				c = 'x';
			text += c;
		}
		return text;
	}

	ClientId randomLive()
	{
		std::set<ClientId>::const_iterator it = live_.begin();
		for (unsigned skip = random_.below(static_cast<unsigned>(live_.size())); skip > 0; --skip)
			++it;
		return *it;
	}

	void markDead(ClientId id)
	{
		live_.erase(id);
		dead_.insert(id);
	}

	void checkEffects()
	{
		core_.takeEffects(effects_);
		for (std::vector<Effect>::size_type i = 0; i < effects_.size() && !failed_; ++i)
		{
			const Effect& effect = effects_[i];
			++effectsChecked_;
			if (live_.count(effect.id) == 0)
			{
				fail("effect for a client that is closed or unknown: " + escape(effect.data));
				return;
			}
			if (effect.kind == Effect::CLOSE)
			{
				++closes_;
				markDead(effect.id);
				continue;
			}
			checkLine(effect.data);
		}
	}

	void checkLine(const std::string& data)
	{
		if (data.size() < 2 || data.compare(data.size() - 2, 2, "\r\n") != 0)
			return fail("line without CRLF: " + escape(data));
		if (data.size() > 512)
			return fail("line longer than 512 bytes: " + escape(data));
		const std::string body = data.substr(0, data.size() - 2);
		if (body.find_first_of("\r\n") != std::string::npos)
			return fail("CR or LF inside a line: " + escape(data));
		if (body.empty())
			return fail("empty line sent");
		if (body[0] != ':' && body.compare(0, 6, "ERROR ") != 0 && body.compare(0, 5, "PING ") != 0)
			return fail("line starts unexpectedly: " + escape(data));
		Message msg;
		if (!Message::parse(body, msg))
			return fail("unparseable line: " + escape(data));
		const bool numeric = msg.command.size() == 3 && msg.command.find_first_not_of("0123456789") == std::string::npos;
		if (numeric && msg.params.empty())
			return fail("numeric reply without a target: " + escape(data));
		++seen_[msg.command];
	}

	void checkDeadStayDead()
	{
		if (dead_.empty())
			return;
		std::set<ClientId>::const_iterator it = dead_.end();
		--it;	// the most recently closed id
		if (core_.wantsInput(*it))
			fail("a closed client still wants input");
	}

	void fail(const std::string& why)
	{
		if (failed_)
			return;
		failed_ = true;
		std::cerr << "FAIL at iteration " << iteration_ << ": " << why
			<< "\n  last action: " << action_.substr(0, 600) << std::endl;
	}

	static std::string escape(const std::string& raw)
	{
		static const char hex[] = "0123456789abcdef";
		std::string out;
		for (std::string::size_type i = 0; i < raw.size() && out.size() < 600; ++i)
		{
			const unsigned char c = static_cast<unsigned char>(raw[i]);
			if (c >= 0x20 && c < 0x7f && c != '\\')
				out += static_cast<char>(c);
			else
			{
				out += "\\x";
				out += hex[c >> 4];
				out += hex[c & 15];
			}
		}
		return out;
	}

	Random random_;
	Core core_;
	std::time_t now_;
	ClientId nextId_;
	std::set<ClientId> live_;
	std::set<ClientId> dead_;
	std::vector<Effect> effects_;
	const std::vector<std::string> nicks_;
	const std::vector<std::string> channels_;
	const std::vector<std::string> commands_;
	std::map<std::string, unsigned long> seen_;
	unsigned long iteration_;
	unsigned long linesFed_;
	unsigned long effectsChecked_;
	unsigned long closes_;
	std::string action_;
	bool failed_;
};

}

int main(int argc, char** argv)
{
	const unsigned long iterations = (argc > 1) ? std::strtoul(argv[1], NULL, 10) : 200000;
	const unsigned seed = (argc > 2) ? static_cast<unsigned>(std::strtoul(argv[2], NULL, 10)) : 1;

	Fuzzer fuzzer(seed);
	const bool ok = fuzzer.run(iterations);
	fuzzer.report();
	std::cout << (ok ? "OK" : "FAILED") << " (seed " << seed << ")" << std::endl;
	return ok ? 0 : 1;
}
