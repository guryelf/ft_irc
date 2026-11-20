#include "Check.hpp"
#include "Harness.hpp"
#include "Limits.hpp"
#include "irc/Text.hpp"

// alice (1), bob (2) and carol (3) are registered; their masks are
// "<nick>!<nick>@127.0.0.1" and their real name is "Real Name".
struct Trio : Harness
{
	Trio()
	{
		registerClient(1, "alice");
		registerClient(2, "bob");
		registerClient(3, "carol");
	}
};

// ---------------------------------------------------------------- JOIN

TEST(joinCreatesChannelWithCreatorAsOperator)
{
	Trio h;
	h.send(1, "JOIN #Room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":alice!alice@127.0.0.1 JOIN #Room\n"
		":ircserv 353 alice = #Room :@alice\n"
		":ircserv 366 alice #Room :End of /NAMES list");
}

TEST(joinIsCaseInsensitiveAndAnnouncedToMembers)
{
	Trio h;
	h.send(1, "JOIN #Room\r\n");
	h.flush();
	h.send(2, "JOIN #ROOM\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2),
		":bob!bob@127.0.0.1 JOIN #Room\n"
		":ircserv 353 bob = #Room :@alice bob\n"
		":ircserv 366 bob #Room :End of /NAMES list");
	CHECK_EQ(h.joinedTo(1), ":bob!bob@127.0.0.1 JOIN #Room");
	CHECK_EQ(h.sentTo(3).size(), 0u);
}

TEST(joinTwiceIsIgnored)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.flush();
	h.send(1, "JOIN #room\r\n");
	h.flush();
	CHECK(h.effects.empty());
}

TEST(joinListHandlesEachChannelSeparately)
{
	Trio h;
	h.send(1, "JOIN #a,bad,#b\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(1), "JOIN 353 366 476 JOIN 353 366");
	const std::vector<std::string> lines = h.sentTo(1);
	if (lines.size() == 7)
		CHECK_EQ(lines[3], ":ircserv 476 alice bad :Bad Channel Mask");
}

TEST(joinTooManyChannels)
{
	Trio h;
	std::string list;
	for (unsigned long i = 0; i <= limits::CHANLIMIT; ++i)
		list += (i > 0 ? ",#c" : "#c") + toString(i);
	h.send(1, "JOIN " + list + "\r\n");
	h.flush();
	const std::vector<std::string> lines = h.sentTo(1);
	CHECK_EQ(lines.size(), 3 * limits::CHANLIMIT + 1);
	CHECK_EQ(lines.back(), ":ircserv 405 alice #c" + toString(limits::CHANLIMIT) + " :You have joined too many channels");
}

TEST(joinZeroLeavesEveryChannel)
{
	Trio h;
	h.send(1, "JOIN #a,#b\r\n");
	h.send(2, "JOIN #a\r\n");
	h.flush();
	h.send(1, "JOIN 0\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 PART #a\n:alice!alice@127.0.0.1 PART #b");
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 PART #a");
	// #b was deleted when it emptied: carol re-creates it and becomes operator.
	h.send(3, "JOIN #b\r\n");
	h.flush();
	CHECK(h.joinedTo(3).find(":ircserv 353 carol = #b :@carol") != std::string::npos);
}

TEST(joinShowsExistingTopic)
{
	Trio h;
	h.send(1, "JOIN #room\r\nTOPIC #room :Hello all\r\n");
	h.flush();
	h.send(2, "JOIN #room\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(2), "JOIN 332 333 353 366");
}

TEST(joinRequiresRegistration)
{
	Harness h;
	h.connect(1);
	h.send(1, "JOIN #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":ircserv 451 * :You have not registered");
}

// ---------------------------------------------------------------- PART

TEST(partIsAnnouncedAndEmptyChannelIsDeleted)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.send(2, "JOIN #room\r\n");
	h.flush();
	h.send(2, "PART #room :see you\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":bob!bob@127.0.0.1 PART #room :see you");
	CHECK_EQ(h.joinedTo(2), ":bob!bob@127.0.0.1 PART #room :see you");

	h.send(1, "PART #room\r\n");
	h.send(3, "JOIN #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 PART #room");
	CHECK(h.joinedTo(3).find("= #room :@carol") != std::string::npos);
}

TEST(partErrors)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.flush();
	h.send(3, "PART #nope,#room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(3),
		":ircserv 403 carol #nope :No such channel\n"
		":ircserv 442 carol #room :You're not on that channel");
}

// ---------------------------------------------------------------- PRIVMSG / NOTICE

TEST(channelMessageReachesOtherMembersOnly)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.send(2, "JOIN #room\r\n");
	h.flush();
	h.send(1, "PRIVMSG #ROOM :hello world\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 PRIVMSG #room :hello world");
	CHECK_EQ(h.sentTo(1).size(), 0u);
	CHECK_EQ(h.sentTo(3).size(), 0u);
}

TEST(privateMessageUsesTheRecipientsNick)
{
	Trio h;
	h.send(1, "PRIVMSG BOB :hi there\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 PRIVMSG bob :hi there");
}

TEST(privmsgErrors)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.flush();
	h.send(3, "PRIVMSG\r\nPRIVMSG bob\r\nPRIVMSG bob :\r\nPRIVMSG #room :x\r\nPRIVMSG nobody,bob,#nochan :x\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(3),
		":ircserv 411 carol :No recipient given (PRIVMSG)\n"
		":ircserv 412 carol :No text to send\n"
		":ircserv 412 carol :No text to send\n"
		":ircserv 404 carol #room :Cannot send to channel\n"
		":ircserv 401 carol nobody :No such nick/channel\n"
		":ircserv 401 carol #nochan :No such nick/channel");
	CHECK_EQ(h.joinedTo(2), ":carol!carol@127.0.0.1 PRIVMSG bob :x");
}

TEST(privmsgToUnregisteredNickFails)
{
	Trio h;
	h.connect(4);
	h.send(4, "NICK dave\r\n");
	h.send(1, "PRIVMSG dave :hi\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":ircserv 401 alice dave :No such nick/channel");
	CHECK_EQ(h.sentTo(4).size(), 0u);
}

TEST(noticeNeverTriggersReplies)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.flush();
	h.send(3, "NOTICE\r\nNOTICE bob\r\nNOTICE nobody :x\r\nNOTICE #room :x\r\nNOTICE bob :psst\r\n");
	h.flush();
	CHECK_EQ(h.sentTo(3).size(), 0u);
	CHECK_EQ(h.joinedTo(2), ":carol!carol@127.0.0.1 NOTICE bob :psst");
}

TEST(ctcpAndDccPassThroughUnchanged)
{
	Trio h;
	const std::string dcc = "\001DCC SEND notes.txt 2130706433 5000 1234\001";
	h.send(1, "PRIVMSG bob :" + dcc + "\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 PRIVMSG bob :" + dcc);
}

// ---------------------------------------------------------------- TOPIC

TEST(topicQuerySetAndClear)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.send(2, "JOIN #room\r\n");
	h.flush();
	h.send(1, "TOPIC #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":ircserv 331 alice #room :No topic is set");

	h.now += 5;
	h.send(2, "TOPIC #room :Weekly sync\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":bob!bob@127.0.0.1 TOPIC #room :Weekly sync");
	CHECK_EQ(h.joinedTo(2), ":bob!bob@127.0.0.1 TOPIC #room :Weekly sync");

	h.send(1, "TOPIC #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 332 alice #room :Weekly sync\n"
		":ircserv 333 alice #room bob 1005");

	h.send(1, "TOPIC #room :\r\nTOPIC #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":alice!alice@127.0.0.1 TOPIC #room :\n"
		":ircserv 331 alice #room :No topic is set");
}

TEST(topicErrorsAndLength)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.flush();
	h.send(3, "TOPIC #nope\r\nTOPIC #room\r\nTOPIC\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(3),
		":ircserv 403 carol #nope :No such channel\n"
		":ircserv 442 carol #room :You're not on that channel\n"
		":ircserv 461 carol TOPIC :Not enough parameters");

	h.send(1, "TOPIC #room :" + std::string(limits::TOPICLEN + 50, 't') + "\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 TOPIC #room :" + std::string(limits::TOPICLEN, 't'));
}

// ---------------------------------------------------------------- shared peers

TEST(quitIsSentOnceToEachPeer)
{
	Trio h;
	h.send(1, "JOIN #a,#b,#solo\r\n");
	h.send(2, "JOIN #a,#b\r\n");
	h.flush();
	h.send(1, "QUIT :bye\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 QUIT :Quit: bye");
	CHECK_EQ(h.sentTo(3).size(), 0u);
	// #solo emptied and is gone: carol re-creates it as operator.
	h.send(3, "JOIN #solo\r\n");
	h.flush();
	CHECK(h.joinedTo(3).find("= #solo :@carol") != std::string::npos);
}

TEST(nickChangeIsSentOnceToEachPeer)
{
	Trio h;
	h.send(1, "JOIN #a,#b\r\n");
	h.send(2, "JOIN #a,#b\r\n");
	h.flush();
	h.send(1, "NICK alicia\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 NICK :alicia");
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 NICK :alicia");
	CHECK_EQ(h.sentTo(3).size(), 0u);
}

TEST(disconnectedClientLeavesChannels)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.send(2, "JOIN #room\r\n");
	h.flush();
	h.core.onClose(1, "Connection closed");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 QUIT :Connection closed");
	h.send(2, "NAMES #room\r\n");
	h.flush();
	CHECK_EQ(h.sentTo(2).at(0), ":ircserv 353 bob = #room :bob");
}

// ---------------------------------------------------------------- NAMES / WHO

TEST(namesCommand)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.send(2, "JOIN #room\r\n");
	h.flush();
	h.send(3, "NAMES #room,#nope\r\nNAMES\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(3),
		":ircserv 353 carol = #room :@alice bob\n"
		":ircserv 366 carol #room :End of /NAMES list\n"
		":ircserv 366 carol #nope :End of /NAMES list\n"
		":ircserv 366 carol * :End of /NAMES list");
}

TEST(namesSplitsLongListsWithinLineLimit)
{
	Harness h;
	const unsigned members = 40;
	for (unsigned i = 0; i < members; ++i)
	{
		std::string nick = "n" + toString(10 + i);
		nick += std::string(limits::NICKLEN - nick.size(), 'x');
		h.registerClient(i + 1, nick);
		h.send(i + 1, "JOIN #big\r\n");
	}
	h.flush();
	h.send(1, "NAMES #big\r\n");
	h.flush();
	const std::vector<std::string> lines = h.sentTo(1);
	CHECK(lines.size() > 2);
	std::size_t names = 0;
	for (std::vector<std::string>::size_type i = 0; i + 1 < lines.size(); ++i)
	{
		CHECK(lines[i].size() + 2 <= limits::MAX_LINE);
		const std::string list = lines[i].substr(lines[i].find(" :") + 2);
		names += 1;
		for (std::string::size_type c = 0; c < list.size(); ++c)
			names += (list[c] == ' ') ? 1 : 0;
	}
	CHECK_EQ(names, static_cast<std::size_t>(members));
	CHECK_EQ(lines.back(), ":ircserv 366 n10xxxxxxxxxxxxxxxxxxxxxxxxxxx #big :End of /NAMES list");
}

TEST(whoForChannelAndNick)
{
	Trio h;
	h.send(1, "JOIN #room\r\n");
	h.send(2, "JOIN #room\r\n");
	h.flush();
	h.send(3, "WHO #room\r\nWHO bob\r\nWHO nobody\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(3),
		":ircserv 352 carol #room alice 127.0.0.1 ircserv alice H@ :0 Real Name\n"
		":ircserv 352 carol #room bob 127.0.0.1 ircserv bob H :0 Real Name\n"
		":ircserv 315 carol #room :End of WHO list\n"
		":ircserv 352 carol * bob 127.0.0.1 ircserv bob H :0 Real Name\n"
		":ircserv 315 carol bob :End of WHO list\n"
		":ircserv 315 carol nobody :End of WHO list");
}
