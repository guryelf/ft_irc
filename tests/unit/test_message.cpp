#include "Check.hpp"
#include "irc/Message.hpp"

TEST(parseSimpleCommand)
{
	Message msg;
	CHECK(Message::parse("NICK alice", msg));
	CHECK_EQ(msg.prefix, "");
	CHECK_EQ(msg.command, "NICK");
	CHECK_EQ(msg.params.size(), 1u);
	if (msg.params.size() == 1)
		CHECK_EQ(msg.params[0], "alice");
}

TEST(parseUppercasesCommand)
{
	Message msg;
	CHECK(Message::parse("privmsg bob :hi", msg));
	CHECK_EQ(msg.command, "PRIVMSG");
}

TEST(parsePrefixAndTrailingWithSpaces)
{
	Message msg;
	CHECK(Message::parse(":alice!a@host PRIVMSG #chan :hello there world", msg));
	CHECK_EQ(msg.prefix, "alice!a@host");
	CHECK_EQ(msg.command, "PRIVMSG");
	CHECK_EQ(msg.params.size(), 2u);
	if (msg.params.size() == 2)
	{
		CHECK_EQ(msg.params[0], "#chan");
		CHECK_EQ(msg.params[1], "hello there world");
	}
}

TEST(parseEmptyTrailing)
{
	Message msg;
	CHECK(Message::parse("TOPIC #chan :", msg));
	CHECK_EQ(msg.params.size(), 2u);
	if (msg.params.size() == 2)
		CHECK_EQ(msg.params[1], "");
}

TEST(parseTrailingKeepsInnerColon)
{
	Message msg;
	CHECK(Message::parse("PRIVMSG bob ::-) ok", msg));
	CHECK_EQ(msg.params.size(), 2u);
	if (msg.params.size() == 2)
		CHECK_EQ(msg.params[1], ":-) ok");
}

TEST(parseToleratesRepeatedAndTrailingSpaces)
{
	Message msg;
	CHECK(Message::parse("USER  guest   0  *  :Real Name", msg));
	CHECK_EQ(msg.params.size(), 4u);
	if (msg.params.size() == 4)
	{
		CHECK_EQ(msg.params[0], "guest");
		CHECK_EQ(msg.params[3], "Real Name");
	}
	CHECK(Message::parse("NICK bob   ", msg));
	CHECK_EQ(msg.params.size(), 1u);
}

TEST(parseFifteenthParamTakesTheRest)
{
	Message msg;
	CHECK(Message::parse("CMD 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16", msg));
	CHECK_EQ(msg.params.size(), 15u);
	if (msg.params.size() == 15)
	{
		CHECK_EQ(msg.params[13], "14");
		CHECK_EQ(msg.params[14], "15 16");
	}
}

TEST(parseRejectsLinesWithoutCommand)
{
	Message msg;
	CHECK(!Message::parse("", msg));
	CHECK(!Message::parse("    ", msg));
	CHECK(!Message::parse(":prefix-only", msg));
	CHECK(!Message::parse(":prefix   ", msg));
}
