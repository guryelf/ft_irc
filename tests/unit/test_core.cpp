#include "Check.hpp"
#include "Harness.hpp"
#include "Limits.hpp"

// These tests send commands that do not exist (COMMAND, FOO, A...), so an
// unregistered client gets ERR_NOTREGISTERED: the reply proves a line
// reached dispatch.
static const char* const NOT_REGISTERED = ":ircserv 451 * :You have not registered";
static const char* const TOO_LONG = ":ircserv 417 * :Input line was too long";

TEST(framingReassemblesSplitCommand)
{
	// The subject's test: "com", "man", "d\n" sent separately with Ctrl+D.
	Harness h;
	h.connect(1);
	h.send(1, "com");
	h.send(1, "man");
	h.flush();
	CHECK(h.effects.empty());
	h.send(1, "d\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), NOT_REGISTERED);
}

TEST(framingAcceptsLfCrAndCrlf)
{
	Harness h;
	h.connect(1);
	h.send(1, "A\nB\rC\r\n");
	h.flush();
	CHECK_EQ(h.sentTo(1).size(), 3u);
}

TEST(framingSkipsEmptyLines)
{
	Harness h;
	h.connect(1);
	h.send(1, "\r\n\n\r\r\n   \r\n");
	h.flush();
	CHECK(h.effects.empty());
}

TEST(framingAcceptsMaximumLine)
{
	Harness h;
	h.connect(1);
	h.send(1, std::string(limits::MAX_LINE - 2, 'A') + "\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), NOT_REGISTERED);
}

TEST(framingRejectsLongLineThenRecovers)
{
	Harness h;
	h.connect(1);
	h.send(1, std::string(limits::MAX_LINE - 1, 'A') + "\r\nNEXT\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), std::string(TOO_LONG) + "\n" + NOT_REGISTERED);
}

TEST(framingDropsUnterminatedLongLineOnce)
{
	// The overflow is detected before the newline arrives, so the buffer
	// never grows past one line however long the client keeps sending.
	Harness h;
	h.connect(1);
	h.send(1, std::string(300, 'A'));
	h.send(1, std::string(300, 'A'));
	h.send(1, std::string(5000, 'A'));
	h.flush();
	CHECK_EQ(h.joinedTo(1), TOO_LONG);
	h.send(1, "tail\r\nNEXT\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), NOT_REGISTERED);
}

TEST(floodControlThrottlesThenResumes)
{
	Harness h;
	h.connect(1);
	std::string lines;
	for (unsigned i = 0; i < limits::FLOOD_BURST + limits::FLOOD_RATE; ++i)
		lines += "FOO\r\n";
	h.send(1, lines);
	h.flush();
	CHECK_EQ(h.sentTo(1).size(), static_cast<std::size_t>(limits::FLOOD_BURST));
	CHECK(!h.core.wantsInput(1));

	h.tick(0);
	h.flush();
	CHECK(h.effects.empty());

	h.tick(1);
	h.flush();
	CHECK_EQ(h.sentTo(1).size(), static_cast<std::size_t>(limits::FLOOD_RATE));
	CHECK(h.core.wantsInput(1));
}

TEST(floodTokensRefillOverTime)
{
	Harness h;
	h.connect(1);
	std::string burst;
	for (unsigned i = 0; i < limits::FLOOD_BURST; ++i)
		burst += "FOO\r\n";
	h.send(1, burst);
	h.now += 60;
	h.send(1, burst);
	h.flush();
	CHECK_EQ(h.sentTo(1).size(), static_cast<std::size_t>(2 * limits::FLOOD_BURST));
	CHECK(h.core.wantsInput(1));
}

TEST(closedClientIsForgotten)
{
	Harness h;
	h.connect(1);
	CHECK(h.core.wantsInput(1));
	h.core.onClose(1, "Connection closed");
	CHECK(!h.core.wantsInput(1));
	h.send(1, "FOO\r\n");
	h.flush();
	CHECK(h.effects.empty());
}

TEST(clientsAreIndependent)
{
	Harness h;
	h.connect(1);
	h.connect(2);
	h.send(1, "com");
	h.send(2, "FOO\r\n");
	h.flush();
	CHECK_EQ(h.sentTo(1).size(), 0u);
	CHECK_EQ(h.joinedTo(2), NOT_REGISTERED);
}

TEST(shutdownSendsErrorAndClosesEveryone)
{
	Harness h;
	h.connect(1);
	h.connect(2);
	h.core.onShutdown();
	h.flush();
	CHECK_EQ(h.joinedTo(1), "ERROR :Closing Link: 127.0.0.1 (Server shutting down)");
	CHECK(h.closed(1));
	CHECK(h.closed(2));
	CHECK(!h.core.wantsInput(1));
}
