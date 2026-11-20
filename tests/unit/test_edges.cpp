// Protocol edge cases: behaviours where the right answer is a design
// decision, pinned down so they cannot change silently.

#include "Check.hpp"
#include "Harness.hpp"

namespace
{

// alice (1) operates #room with bob (2) in it; carol (3) is outside.
struct EdgeRoom : Harness
{
	EdgeRoom()
	{
		registerClient(1, "alice");
		registerClient(2, "bob");
		registerClient(3, "carol");
		send(1, "JOIN #room\r\n");
		send(2, "JOIN #room\r\n");
		flush();
	}
};

}

TEST(edgeJoinKeysMatchChannelsByPosition)
{
	EdgeRoom h;
	h.send(1, "JOIN #a,#b\r\nMODE #a +k ka\r\nMODE #b +k kb\r\n");
	h.flush();
	h.send(3, "JOIN #a,#b ka\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(3), "JOIN 353 366 475");
}

TEST(edgeModeMissingParameterKeepsEarlierChanges)
{
	EdgeRoom h;
	h.send(1, "MODE #room +kl key\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 461 alice MODE :Not enough parameters\n"
		":alice!alice@127.0.0.1 MODE #room +k key");
}

TEST(edgeModeOpThenDeopSameNick)
{
	EdgeRoom h;
	h.send(1, "MODE #room +o-o bob bob\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 MODE #room +o-o bob bob");
	h.send(2, "KICK #room alice\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(2), "482");
}

TEST(edgeCapAfterRegistrationDoesNotBlock)
{
	EdgeRoom h;
	h.send(1, "CAP LS 302\r\nPING :still\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":ircserv CAP alice LS :\n:ircserv PONG ircserv :still");
}

TEST(edgeMessageToSelfIsDelivered)
{
	EdgeRoom h;
	h.send(1, "PRIVMSG alice :note to self\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 PRIVMSG alice :note to self");
}

TEST(edgeInviteDoesNotLiftUserLimit)
{
	EdgeRoom h;
	h.send(1, "MODE #room +il 2\r\nINVITE carol #room\r\n");
	h.flush();
	h.send(3, "JOIN #room\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(3), "471");
}

TEST(edgeNickChangeShowsInNames)
{
	EdgeRoom h;
	h.send(1, "NICK al\r\n");
	h.flush();
	h.send(2, "NAMES #room\r\n");
	h.flush();
	CHECK_EQ(h.sentTo(2).at(0), ":ircserv 353 bob = #room :@al bob");
}

TEST(edgeCommandsAreCaseInsensitive)
{
	EdgeRoom h;
	h.send(1, "privmsg #ROOM :lower\r\nPrivMsg #Room :mixed\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2),
		":alice!alice@127.0.0.1 PRIVMSG #room :lower\n"
		":alice!alice@127.0.0.1 PRIVMSG #room :mixed");
}

TEST(edgeClientPrefixIsIgnored)
{
	EdgeRoom h;
	h.send(1, ":evil!root@server PRIVMSG bob :hi\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 PRIVMSG bob :hi");
}

TEST(edgeTopicRestrictionFollowsOperatorStatus)
{
	EdgeRoom h;
	h.send(1, "MODE #room +t\r\nMODE #room +o bob\r\n");
	h.send(2, "TOPIC #room :as operator\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(2), "MODE MODE TOPIC");
	h.send(1, "MODE #room -o bob\r\n");
	h.send(2, "TOPIC #room :as member\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(2), "MODE 482");
}

TEST(edgeOperatorCanKickAnotherOperator)
{
	EdgeRoom h;
	h.send(1, "MODE #room +o bob\r\n");
	h.send(2, "KICK #room alice :coup\r\n");
	h.flush();
	CHECK_EQ(h.sentTo(1).back(), ":bob!bob@127.0.0.1 KICK #room alice :coup");
}

TEST(edgeNickFreedWhenUnregisteredClientCloses)
{
	Harness h;
	h.connect(1);
	h.send(1, "NICK alice\r\n");
	h.core.onClose(1, "Connection closed");
	h.registerClient(2, "alice");
	CHECK_EQ(h.codesTo(2), "001 002 003 004 005 422");
}

TEST(edgeJoinZeroWithoutChannelsIsSilent)
{
	Harness h;
	h.registerClient(1, "alice");
	h.send(1, "JOIN 0\r\n");
	h.flush();
	CHECK(h.effects.empty());
}

TEST(edgeEmptyTargetInListIsReportedAsStar)
{
	EdgeRoom h;
	h.send(1, "PRIVMSG bob,,carol :x\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":ircserv 401 alice * :No such nick/channel");
	CHECK_EQ(h.sentTo(2).size(), 1u);
	CHECK_EQ(h.sentTo(3).size(), 1u);
}
