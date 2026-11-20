#include "Check.hpp"
#include "Harness.hpp"

// alice (1) created #room and is its operator; bob (2) is a member;
// carol (3) is registered but outside the channel.
struct Room : Harness
{
	Room()
	{
		registerClient(1, "alice");
		registerClient(2, "bob");
		registerClient(3, "carol");
		send(1, "JOIN #room\r\n");
		send(2, "JOIN #room\r\n");
		flush();
	}
};

// ---------------------------------------------------------------- KICK

TEST(kickRemovesMemberAndTellsEveryone)
{
	Room h;
	h.send(1, "KICK #room bob :behave\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 KICK #room bob :behave");
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 KICK #room bob :behave");
	h.send(2, "PRIVMSG #room :still here?\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(2), "404");
}

TEST(kickDefaultCommentIsKickerNick)
{
	Room h;
	h.send(1, "KICK #room bob\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 KICK #room bob :alice");
}

TEST(kickErrors)
{
	Room h;
	h.send(2, "KICK #room alice\r\n");
	h.send(3, "KICK #room bob\r\n");
	h.send(1, "KICK #nope bob\r\nKICK #room carol\r\nKICK #room nobody\r\nKICK #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":ircserv 482 bob #room :You're not channel operator");
	CHECK_EQ(h.joinedTo(3), ":ircserv 442 carol #room :You're not on that channel");
	CHECK_EQ(h.joinedTo(1),
		":ircserv 403 alice #nope :No such channel\n"
		":ircserv 441 alice carol #room :They aren't on that channel\n"
		":ircserv 401 alice nobody :No such nick/channel\n"
		":ircserv 461 alice KICK :Not enough parameters");
}

TEST(kickListHandlesEachTargetSeparately)
{
	Room h;
	h.send(3, "JOIN #room\r\n");
	h.flush();
	h.send(1, "KICK #room bob,nobody,carol :out\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":alice!alice@127.0.0.1 KICK #room bob :out\n"
		":ircserv 401 alice nobody :No such nick/channel\n"
		":alice!alice@127.0.0.1 KICK #room carol :out");
}

TEST(kickingYourselfFromLastSeatStopsSafely)
{
	// alice kicks herself out of a channel where she is alone: the channel
	// is deleted mid-command, and the remaining targets must not touch it.
	Room h;
	h.send(2, "PART #room\r\n");
	h.flush();
	h.send(1, "KICK #room alice,bob\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 KICK #room alice :alice");
	h.send(3, "JOIN #room\r\n");
	h.flush();
	CHECK(h.joinedTo(3).find("= #room :@carol") != std::string::npos);
}

// ---------------------------------------------------------------- INVITE

TEST(inviteLetsTargetIntoInviteOnlyChannelOnce)
{
	Room h;
	h.send(1, "MODE #room +i\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 MODE #room +i");

	h.send(3, "JOIN #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(3), ":ircserv 473 carol #room :Cannot join channel (+i)");

	h.send(1, "INVITE carol #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":ircserv 341 alice carol #room");
	CHECK_EQ(h.joinedTo(3), ":alice!alice@127.0.0.1 INVITE carol #room");

	h.send(3, "JOIN #room\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(3), "JOIN 353 366");

	// The invite was used up by the JOIN.
	h.send(3, "PART #room\r\nJOIN #room\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(3), "PART 473");
}

TEST(inviteErrors)
{
	Room h;
	h.send(1, "INVITE nobody #room\r\nINVITE carol #nope\r\nINVITE bob #room\r\n");
	h.send(3, "INVITE bob #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 401 alice nobody :No such nick/channel\n"
		":ircserv 403 alice #nope :No such channel\n"
		":ircserv 443 alice bob #room :is already on channel");
	CHECK_EQ(h.joinedTo(3), ":ircserv 442 carol #room :You're not on that channel");
}

TEST(onlyOperatorsInviteToInviteOnlyChannel)
{
	Room h;
	h.send(2, "INVITE carol #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":ircserv 341 bob carol #room");

	h.send(1, "MODE #room +i\r\n");
	h.flush();
	h.send(2, "INVITE carol #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":ircserv 482 bob #room :You're not channel operator");
}

// ---------------------------------------------------------------- MODE: queries

TEST(modeQueryShowsKeyToMembersOnly)
{
	Room h;
	h.send(1, "MODE #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":ircserv 324 alice #room +");

	h.send(1, "MODE #room +tkl secret 5\r\nMODE #room\r\n");
	h.send(3, "MODE #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":alice!alice@127.0.0.1 MODE #room +tkl secret 5\n"
		":ircserv 324 alice #room +tkl secret 5");
	CHECK_EQ(h.joinedTo(3), ":ircserv 324 carol #room +tkl");
}

TEST(modeBanListQueryIsEmpty)
{
	Room h;
	h.send(1, "MODE #room b\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":ircserv 368 alice #room :End of channel ban list");
}

// ---------------------------------------------------------------- MODE: changes

TEST(modeChangesAreAnnouncedInOneLine)
{
	Room h;
	h.send(1, "MODE #room +ik secret\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 MODE #room +ik secret");

	h.send(1, "MODE #room +o-k bob\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 MODE #room +o-k bob *");
}

TEST(modeWithoutRealChangeIsSilent)
{
	Room h;
	h.send(1, "MODE #room +i\r\n");
	h.flush();
	h.send(1, "MODE #room +i-l-k\r\n");
	h.flush();
	CHECK(h.effects.empty());
}

TEST(modeOperatorGrantAndRevoke)
{
	Room h;
	h.send(1, "MODE #room +o bob\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 MODE #room +o bob");
	h.send(2, "NAMES #room\r\n");
	h.flush();
	CHECK_EQ(h.sentTo(2).at(0), ":ircserv 353 bob = #room :@alice @bob");

	h.send(2, "MODE #room -o alice\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":bob!bob@127.0.0.1 MODE #room -o alice");
	h.send(1, "KICK #room bob\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(1), "482");
}

TEST(modeOperatorErrors)
{
	Room h;
	h.send(1, "MODE #room +o nobody\r\nMODE #room +o carol\r\nMODE #room +o\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 401 alice nobody :No such nick/channel\n"
		":ircserv 441 alice carol #room :They aren't on that channel\n"
		":ircserv 461 alice MODE :Not enough parameters");
}

TEST(modeKeyIsRequiredToJoin)
{
	Room h;
	h.send(1, "MODE #room +k secret\r\n");
	h.flush();
	h.send(3, "JOIN #room\r\nJOIN #room wrong\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(3), "475 475");
	h.send(3, "JOIN #room secret\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(3), "JOIN 353 366");

	h.send(3, "PART #room\r\n");
	h.send(1, "MODE #room -k\r\n");
	h.flush();
	h.send(3, "JOIN #room\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(3), "JOIN 353 366");
}

TEST(modeRejectsMalformedKeys)
{
	Room h;
	h.send(1, "MODE #room +k a,b\r\nMODE #room +k :a b\r\nMODE #room +k\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 525 alice #room :Key is not well-formed\n"
		":ircserv 525 alice #room :Key is not well-formed\n"
		":ircserv 461 alice MODE :Not enough parameters");
}

TEST(modeLimitCapsMembers)
{
	Room h;
	h.send(1, "MODE #room +l abc\r\nMODE #room +l 0\r\n");
	h.flush();
	CHECK(h.effects.empty());

	h.send(1, "MODE #room +l 2\r\n");
	h.send(3, "JOIN #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 MODE #room +l 2");
	CHECK_EQ(h.joinedTo(3), ":ircserv 471 carol #room :Cannot join channel (+l)");

	// carol is not a member, so she does not see the -l itself.
	h.send(1, "MODE #room -l\r\n");
	h.send(3, "JOIN #room\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1), ":alice!alice@127.0.0.1 MODE #room -l\n:carol!carol@127.0.0.1 JOIN #room");
	CHECK_EQ(h.codesTo(3), "JOIN 353 366");
}

TEST(modeTopicRestriction)
{
	Room h;
	h.send(1, "MODE #room +t\r\n");
	h.send(2, "TOPIC #room :bob's topic\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(2), "MODE 482");

	h.send(1, "TOPIC #room :ops only\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":alice!alice@127.0.0.1 TOPIC #room :ops only");

	h.send(1, "MODE #room -t\r\n");
	h.send(2, "TOPIC #room :anyone\r\n");
	h.flush();
	CHECK_EQ(h.codesTo(2), "MODE TOPIC");
}

TEST(modeErrors)
{
	Room h;
	h.send(2, "MODE #room +i\r\n");
	h.send(3, "MODE #room +i\r\n");
	h.send(1, "MODE #nope +i\r\nMODE #room +x\r\nMODE #room +b someone\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(2), ":ircserv 482 bob #room :You're not channel operator");
	CHECK_EQ(h.joinedTo(3), ":ircserv 442 carol #room :You're not on that channel");
	CHECK_EQ(h.joinedTo(1),
		":ircserv 403 alice #nope :No such channel\n"
		":ircserv 472 alice x :is unknown mode char to me\n"
		":ircserv 472 alice b :is unknown mode char to me");
}

TEST(modeBadFlagDoesNotCancelOthers)
{
	Room h;
	h.send(1, "MODE #room +xi\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 472 alice x :is unknown mode char to me\n"
		":alice!alice@127.0.0.1 MODE #room +i");
}

// ---------------------------------------------------------------- user MODE

TEST(userModeInvisible)
{
	Room h;
	h.send(1, "MODE alice\r\nMODE alice +i\r\nMODE ALICE\r\nMODE alice +i\r\nMODE alice -i\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 221 alice +\n"
		":alice!alice@127.0.0.1 MODE alice :+i\n"
		":ircserv 221 alice +i\n"
		":alice!alice@127.0.0.1 MODE alice :-i");
}

TEST(userModeErrors)
{
	Room h;
	h.send(1, "MODE alice +x\r\nMODE bob +i\r\nMODE nobody\r\n");
	h.flush();
	CHECK_EQ(h.joinedTo(1),
		":ircserv 501 alice :Unknown MODE flag\n"
		":ircserv 502 alice :Cant change mode for other users\n"
		":ircserv 401 alice nobody :No such nick/channel");
}
