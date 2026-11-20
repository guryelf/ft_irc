#include "Check.hpp"
#include "irc/Channel.hpp"

TEST(channelOperatorsStayMembers)
{
	Channel channel("#a");

	channel.addMember(1, true);
	channel.addMember(2, false);
	CHECK(channel.isOperator(1));
	CHECK(!channel.isOperator(2));
	CHECK_EQ(channel.size(), 2u);

	channel.removeMember(1);
	CHECK(!channel.isMember(1));
	CHECK(!channel.isOperator(1));

	// Only members can be operators.
	channel.setOperator(3, true);
	CHECK(!channel.isOperator(3));
	channel.setOperator(2, true);
	CHECK(channel.isOperator(2));
	channel.setOperator(2, false);
	CHECK(!channel.isOperator(2));
}

TEST(channelJoinConsumesInvite)
{
	Channel channel("#a");

	channel.invite(5);
	CHECK(channel.isInvited(5));
	channel.addMember(5, false);
	CHECK(!channel.isInvited(5));
}

TEST(channelEmptyAfterLastMemberLeaves)
{
	Channel channel("#a");

	CHECK(channel.empty());
	channel.addMember(1, true);
	CHECK(!channel.empty());
	channel.removeMember(1);
	CHECK(channel.empty());
}

TEST(channelModeString)
{
	Channel channel("#a");

	CHECK_EQ(channel.modes(true), "+");
	channel.inviteOnly = true;
	channel.topicRestricted = true;
	channel.key = "secret";
	channel.limit = 5;
	CHECK_EQ(channel.modes(true), "+itkl secret 5");
	CHECK_EQ(channel.modes(false), "+itkl");
}
