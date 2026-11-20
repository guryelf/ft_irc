#include "Check.hpp"
#include "Limits.hpp"
#include "irc/Text.hpp"

TEST(ircLowerUsesRfc1459Mapping)
{
	CHECK_EQ(ircLower("NiCk[]\\~"), "nick{}|^");
	CHECK_EQ(ircLower("#Chan"), "#chan");
	CHECK_EQ(ircLower("a-1_`"), "a-1_`");
}

TEST(validNicks)
{
	CHECK(isValidNick("alice"));
	CHECK(isValidNick("[bot]"));
	CHECK(isValidNick("a-b_c9"));
	CHECK(isValidNick("`x"));
	CHECK(isValidNick(std::string(limits::NICKLEN, 'n')));
}

TEST(invalidNicks)
{
	CHECK(!isValidNick(""));
	CHECK(!isValidNick("1abc"));
	CHECK(!isValidNick("-abc"));
	CHECK(!isValidNick("a b"));
	CHECK(!isValidNick("a,b"));
	CHECK(!isValidNick("a@b"));
	CHECK(!isValidNick(std::string(limits::NICKLEN + 1, 'n')));
	CHECK(!isValidNick(std::string("a\0b", 3)));
}

TEST(validChannelNames)
{
	CHECK(isValidChannelName("#a"));
	CHECK(isValidChannelName("&local"));
	CHECK(isValidChannelName("#42-paris.ft_irc"));
	CHECK(isValidChannelName("#" + std::string(limits::CHANNELLEN - 1, 'c')));
}

TEST(invalidChannelNames)
{
	CHECK(!isValidChannelName(""));
	CHECK(!isValidChannelName("#"));
	CHECK(!isValidChannelName("chan"));
	CHECK(!isValidChannelName("#a b"));
	CHECK(!isValidChannelName("#a,b"));
	CHECK(!isValidChannelName("#a:b"));
	CHECK(!isValidChannelName("#a\ab"));
	CHECK(!isValidChannelName("#" + std::string(limits::CHANNELLEN, 'c')));
}

TEST(splitCommaKeepsEmptyItems)
{
	CHECK_EQ(splitComma("").size(), 0u);

	std::vector<std::string> one = splitComma("#a");
	CHECK_EQ(one.size(), 1u);

	std::vector<std::string> three = splitComma("#a,,#b");
	CHECK_EQ(three.size(), 3u);
	if (three.size() == 3)
	{
		CHECK_EQ(three[0], "#a");
		CHECK_EQ(three[1], "");
		CHECK_EQ(three[2], "#b");
	}

	CHECK_EQ(splitComma("#a,").size(), 2u);
}

TEST(toStringFormatsNumbers)
{
	CHECK_EQ(toString(0), "0");
	CHECK_EQ(toString(65535), "65535");
}
