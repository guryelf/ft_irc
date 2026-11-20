#include "Check.hpp"
#include "Harness.hpp"
#include "Limits.hpp"

static const char* const WELCOME_CODES = "001 002 003 004 005 422";
static const char* const BAD_PASSWORD_CLOSE = "ERROR :Closing Link: 127.0.0.1 (Bad password)";

// ---------------------------------------------------------------- registration

TEST(registrationSendsWelcomeBurst)
{
	Harness h;
	h.connect(1);
	h.send(1, "PASS secret\r\nNICK alice\r\nUSER alice 0 * :Alice A\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(1), WELCOME_CODES);
	const std::vector<std::string> lines = h.sentTo(1);
	if (lines.size() == 6)
	{
		CHECK_EQ(lines[0], ":ircserv 001 alice :Welcome to the Internet Relay Network alice!alice@127.0.0.1");
		CHECK_EQ(lines[2], ":ircserv 003 alice :This server was created 1970-01-01 00:16:40 UTC");
		CHECK_EQ(lines[3], ":ircserv 004 alice ircserv ircserv-1.0 i iklot");
		CHECK(lines[4].find(" CASEMAPPING=rfc1459 ") != std::string::npos);
		CHECK(lines[4].find(" CHANMODES=,k,l,it ") != std::string::npos);
		CHECK(lines[4].find(" PREFIX=(o)@ ") != std::string::npos);
		CHECK_EQ(lines[5], ":ircserv 422 alice :MOTD File is missing");
	}
}

TEST(registrationAcceptsPassInAnyPositionBeforeCompletion)
{
	Harness h;
	h.connect(1);
	h.send(1, "NICK alice\r\nPASS secret\r\nUSER alice 0 * :A\r\n");
	h.connect(2);
	h.send(2, "USER bob 0 * :B\r\nPASS secret\r\nNICK bob\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(1), WELCOME_CODES);
	CHECK_EQ(h.codesTo(2), WELCOME_CODES);
}

TEST(registrationWithoutPasswordIsRefused)
{
	// The password is checked when NICK and USER are both in: a PASS after
	// that is too late, and the client is told at once instead of hanging.
	Harness h;
	h.connect(1);
	h.send(1, "NICK alice\r\nUSER alice 0 * :A\r\nPASS secret\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), std::string(":ircserv 464 alice :Password required\n") + BAD_PASSWORD_CLOSE);
	CHECK(h.closed(1));
	CHECK(!h.core.wantsInput(1));
}

TEST(wrongPasswordDisconnectsImmediately)
{
	Harness h;
	h.connect(1);
	h.send(1, "PASS nope\r\nNICK alice\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), std::string(":ircserv 464 * :Password incorrect\n") + BAD_PASSWORD_CLOSE);
	CHECK(h.closed(1));
}

TEST(reregistrationIsRefused)
{
	Harness h;
	h.registerClient(1, "alice");
	h.send(1, "PASS secret\r\nUSER x 0 * :y\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 462 alice :You may not reregister\n"
		":ircserv 462 alice :You may not reregister");
}

TEST(userNameIsSanitized)
{
	Harness h;
	h.connect(1);
	h.send(1, "PASS secret\r\nNICK alice\r\nUSER a@b!c 0 * :A\r\n");
	h.connect(2);
	h.send(2, "PASS secret\r\nNICK bob\r\nUSER abcdefghijklmnop 0 * :B\r\n");
	h.flush();
	CHECK_EQ(h.sentTo(1).at(0), ":ircserv 001 alice :Welcome to the Internet Relay Network alice!a_b_c@127.0.0.1");
	CHECK_EQ(h.sentTo(2).at(0), ":ircserv 001 bob :Welcome to the Internet Relay Network bob!abcdefghij@127.0.0.1");
}

// ---------------------------------------------------------------- CAP

TEST(capNegotiationHoldsRegistrationUntilEnd)
{
	Harness h;
	h.connect(1);
	h.send(1, "CAP LS 302\r\nPASS secret\r\nNICK alice\r\nUSER alice 0 * :A\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":ircserv CAP * LS :");
	h.send(1, "CAP END\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(1), WELCOME_CODES);
}

TEST(capSubcommands)
{
	Harness h;
	h.connect(1);
	h.send(1, "CAP REQ :multi-prefix\r\nCAP list\r\nCAP FOO\r\nCAP\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv CAP * NAK :multi-prefix\n"
		":ircserv CAP * LIST :\n"
		":ircserv 410 * FOO :Invalid CAP command\n"
		":ircserv 461 * CAP :Not enough parameters");
}

// ---------------------------------------------------------------- NICK

TEST(nickErrors)
{
	Harness h;
	h.connect(1);
	h.send(1, "NICK\r\nNICK 1abc\r\nNICK :a b\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 431 * :No nickname given\n"
		":ircserv 432 * 1abc :Erroneous nickname\n"
		":ircserv 432 * * :Erroneous nickname");
}

TEST(nickInUseIgnoresCase)
{
	Harness h;
	h.registerClient(1, "[a]");
	h.connect(2);
	h.send(2, "NICK {A}\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":ircserv 433 * {A} :Nickname is already in use");
}

TEST(nickReservedBeforeRegistration)
{
	Harness h;
	h.connect(1);
	h.send(1, "NICK alice\r\n");
	h.connect(2);
	h.send(2, "NICK ALICE\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":ircserv 433 * ALICE :Nickname is already in use");
}

TEST(nickChangeAfterRegistration)
{
	Harness h;
	h.registerClient(1, "alice");
	h.send(1, "NICK bob\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 NICK :bob");

	// The old nick is free again; the new one is taken.
	h.registerClient(2, "alice");
	CHECK_EQ(h.codesTo(2), WELCOME_CODES);
	h.connect(3);
	h.send(3, "NICK bob\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(3), "433");
}

TEST(nickSameIsIgnoredButCaseChangeIsAllowed)
{
	Harness h;
	h.registerClient(1, "alice");
	h.send(1, "NICK alice\r\n");
	h.flush();
	CHECK(h.effects.empty());
	h.send(1, "NICK Alice\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 NICK :Alice");
}

// ---------------------------------------------------------------- dispatcher

TEST(registeredClientGetsUnknownCommandAndNeedMoreParams)
{
	Harness h;
	h.registerClient(1, "alice");
	h.send(1, "FOO bar\r\nUSER a b\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 421 alice FOO :Unknown command\n"
		":ircserv 461 alice USER :Not enough parameters");
}

// ---------------------------------------------------------------- PING / PONG / QUIT

TEST(pingIsAnswered)
{
	Harness h;
	h.connect(1);
	h.send(1, "PING token\r\nPING\r\nPONG whatever\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv PONG ircserv :token\n"
		":ircserv 409 * :No origin specified");
}

TEST(quitClosesAndStopsProcessing)
{
	Harness h;
	h.registerClient(1, "alice");
	h.send(1, "QUIT :bye now\r\nNICK other\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), "ERROR :Closing Link: 127.0.0.1 (Quit: bye now)");
	CHECK(h.closed(1));

	h.registerClient(2, "bob");
	h.send(2, "QUIT\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), "ERROR :Closing Link: 127.0.0.1 (Client Quit)");
}

TEST(nickIsFreedWhenClientLeaves)
{
	Harness h;
	h.registerClient(1, "alice");
	h.send(1, "QUIT\r\n");
	h.registerClient(2, "alice");
	CHECK_EQ(h.codesTo(2), WELCOME_CODES);

	h.core.onClose(2, "Connection closed");
	h.registerClient(3, "alice");
	CHECK_EQ(h.codesTo(3), WELCOME_CODES);
}

// ---------------------------------------------------------------- liveness

TEST(registrationTimeout)
{
	Harness h;
	h.connect(1);
	h.send(1, "NICK alice\r\n");
	h.tick(limits::REG_TIMEOUT - 1);
	h.flush();
	CHECK(h.effects.empty());
	h.tick(1);
	h.flush();
	CHECK_EQ(h.joinedTo(1), "ERROR :Closing Link: 127.0.0.1 (Registration timeout)");
	CHECK(h.closed(1));
}

TEST(idleClientIsPingedThenDropped)
{
	Harness h;
	h.registerClient(1, "alice");
	h.tick(limits::PING_IDLE - 1);
	h.flush();
	CHECK(h.effects.empty());
	h.tick(1);
	h.flush();
	CHECK_EQ(h.joinedTo(1), "PING :ircserv");
	h.tick(limits::PING_TIMEOUT - 1);
	h.flush();
	CHECK(h.effects.empty());
	h.tick(1);
	h.flush();
	CHECK_EQ(h.joinedTo(1), "ERROR :Closing Link: 127.0.0.1 (Ping timeout: 120 seconds)");
	CHECK(h.closed(1));
}

TEST(pongKeepsClientAlive)
{
	Harness h;
	h.registerClient(1, "alice");
	h.tick(limits::PING_IDLE);
	h.send(1, "PONG :ircserv\r\n");
	h.tick(limits::PING_TIMEOUT);
	h.flush();
	CHECK_EQ(h.joinedTo(1), "PING :ircserv");
	CHECK(!h.closed(1));
	// Idle again for PING_IDLE seconds since the PONG: a new PING.
	h.tick(limits::PING_IDLE - limits::PING_TIMEOUT);
	h.flush();
	CHECK_EQ(h.joinedTo(1), "PING :ircserv");
}
