#include <csignal>
#include <cstring>
#include <ctime>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "Log.hpp"
#include "irc/Core.hpp"
#include "net/Reactor.hpp"

namespace
{

volatile std::sig_atomic_t g_stopRequested = 0;

}

// Only sets a flag: the event loop sees it after poll() returns with EINTR.
extern "C" void handleStopSignal(int)
{
	g_stopRequested = 1;
}

namespace
{

// A port is 1 to 5 decimal digits with a value in [1, 65535].
// Parsed by hand so overflow and trailing garbage ("6667abc") are rejected.
bool isValidPort(const std::string& text)
{
	if (text.empty() || text.size() > 5)
		return false;
	unsigned long value = 0;
	for (std::string::size_type i = 0; i < text.size(); ++i)
	{
		if (text[i] < '0' || text[i] > '9')
			return false;
		value = value * 10 + static_cast<unsigned long>(text[i] - '0');
	}
	return value >= 1 && value <= 65535;
}

// Clients send the password as one PASS parameter, so it must be non-empty
// and free of whitespace, or no client could ever send it correctly.
bool isValidPassword(const std::string& password)
{
	if (password.empty())
		return false;
	return password.find_first_of(" \t\r\n") == std::string::npos;
}

void installSignalHandlers()
{
	struct sigaction action;

	std::memset(&action, 0, sizeof(action));
	action.sa_handler = handleStopSignal;
	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;	// no SA_RESTART: the signal interrupts poll() right away
	if (sigaction(SIGINT, &action, NULL) < 0 || sigaction(SIGTERM, &action, NULL) < 0)
		throw std::runtime_error("cannot install SIGINT/SIGTERM handlers");
	// A peer that resets the connection would otherwise kill the process on
	// the next send(). With SIGPIPE ignored, send() returns -1 instead.
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		throw std::runtime_error("cannot ignore SIGPIPE");
}

}

int main(int argc, char** argv)
{
	logOpen();
	if (argc != 3)
	{
		std::cerr << "usage: ./ircserv <port> <password>" << std::endl;
		return 1;
	}
	try
	{
		const std::string port(argv[1]);
		const std::string password(argv[2]);

		if (!isValidPort(port))
		{
			std::cerr << "ircserv: invalid port '" << port << "' (expected 1-65535)" << std::endl;
			return 1;
		}
		if (!isValidPassword(password))
		{
			std::cerr << "ircserv: invalid password (must be non-empty, without whitespace)" << std::endl;
			return 1;
		}
		installSignalHandlers();
		Core core(password, std::time(NULL));
		Reactor reactor(core, port);
		logInfo("listening on port " + port);
		reactor.run(g_stopRequested);
		return 0;
	}
	catch (const std::exception& e)
	{
		std::cerr << "ircserv: fatal: " << e.what() << std::endl;
		return 1;
	}
}
