#ifndef CORE_HPP
#define CORE_HPP

#include <cstddef>
#include <ctime>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "irc/Channel.hpp"
#include "irc/Client.hpp"
#include "irc/Effect.hpp"
#include "irc/IrcError.hpp"
#include "irc/Message.hpp"
#include "irc/Numerics.hpp"

// The IRC protocol as a pure state machine (sans-I/O).
//
// The network shell reports events through the on*() entry points, then
// drains the resulting Effects with takeEffects() and carries them out.
// The core makes no system calls and never keeps a Client reference across
// entry points: clients are looked up by ClientId every time.
class Core
{
public:
	Core(const std::string& password, std::time_t startedAt);

	void onConnect(ClientId id, const std::string& host, std::time_t now);
	void onData(ClientId id, const std::string& bytes, std::time_t now);
	// The shell lost the connection. Forget the client; no CLOSE effect follows.
	void onClose(ClientId id, const std::string& reason);
	// Called about once per second: resumes throttled input, enforces timeouts.
	void onTick(std::time_t now);
	// Sends ERROR to every client and asks the shell to close them all.
	void onShutdown();

	// False while the client is throttled or gone: the shell stops reading it.
	bool wantsInput(ClientId id) const;
	// Moves every pending Effect into out, in the order they were produced.
	void takeEffects(std::vector<Effect>& out);

private:
	struct CommandSpec
	{
		typedef void (Core::*Handler)(Client&, const Message&);

		Handler handler;
		std::size_t minParams;		// fewer gives ERR_NEEDMOREPARAMS
		bool needsRegistration;		// unregistered gives ERR_NOTREGISTERED

		CommandSpec();
		CommandSpec(Handler commandHandler, std::size_t requiredParams, bool registeredOnly);
	};

	typedef std::map<ClientId, Client> ClientMap;
	typedef std::map<std::string, ClientId> NickMap;		// key: ircLower(nick)
	typedef std::map<std::string, Channel> ChannelMap;		// key: ircLower(name)
	typedef std::map<std::string, CommandSpec> CommandMap;	// key: upper-case command

	// Not copyable: one core owns the whole server state.
	Core(const Core&);
	Core& operator=(const Core&);

	// Input pipeline: bytes -> lines -> Message -> handler (Core.cpp)
	void processInput(ClientId id, std::time_t now);
	void refillFlood(Client& client, std::time_t now);
	void dispatch(Client& client, const Message& msg);
	void checkLiveness(std::time_t now);

	// State (Core.cpp)
	Client* findClient(ClientId id);
	void setNick(Client& client, const std::string& nick);
	// Everyone sharing at least one channel with client, client excluded.
	std::set<ClientId> peersOf(const Client& client) const;
	// Removes the client everywhere and sends QUIT to its peers.
	// The only place a client leaves the core.
	void removeClient(ClientId id, const std::string& quitReason);
	// Core-initiated close: ERROR to the client, QUIT to peers, then CLOSE.
	// The Client is destroyed: callers must not touch it afterwards.
	void disconnect(ClientId id, const std::string& reason);

	// Lookups and checks shared by commands (Core.cpp). The require*
	// functions throw IrcError, which dispatch() turns into a reply.
	Client* findClientByNick(const std::string& nick);	// registered clients only
	Channel* findChannel(const std::string& name);
	Channel& requireChannel(const std::string& name);	// else ERR_NOSUCHCHANNEL
	void requireMember(const Channel& channel, const Client& client) const;	// else ERR_NOTONCHANNEL
	void requireOperator(const Channel& channel, const Client& client) const;	// else ERR_CHANOPRIVSNEEDED
	// Drops one membership on both sides; deletes the channel once empty.
	void leaveChannel(Client& client, const std::string& channelKey);

	// Output (Core.cpp). Every line goes through sendLine, which caps the
	// length and adds CRLF.
	void sendLine(ClientId id, const std::string& line);
	void sendToAll(const std::set<ClientId>& ids, const std::string& line);
	void broadcast(const Channel& channel, const std::string& line);
	void broadcastExcept(const Channel& channel, const std::string& line, ClientId except);
	void reply(const Client& client, Numeric code, const std::string& params);
	void reply(const Client& client, Numeric code, const std::string& params, const std::string& text);
	void replyError(const Client& client, const IrcError& error);
	void sendTopic(const Client& client, const Channel& channel);	// 332 + 333
	void sendNames(const Client& client, const Channel& channel);	// 353... + 366

	// Registration and connection commands (cmd/Registration.cpp)
	void cmdCap(Client& client, const Message& msg);
	void cmdPass(Client& client, const Message& msg);
	void cmdNick(Client& client, const Message& msg);
	void cmdUser(Client& client, const Message& msg);
	void cmdPing(Client& client, const Message& msg);
	void cmdPong(Client& client, const Message& msg);
	void cmdQuit(Client& client, const Message& msg);
	void tryCompleteRegistration(Client& client);
	void sendWelcome(const Client& client);

	// Channel commands (cmd/Channels.cpp)
	void cmdJoin(Client& client, const Message& msg);
	void cmdPart(Client& client, const Message& msg);
	void cmdTopic(Client& client, const Message& msg);
	void cmdNames(Client& client, const Message& msg);
	void cmdWho(Client& client, const Message& msg);
	void joinChannel(Client& client, const std::string& name, const std::string& key);
	void sendWhoReply(const Client& client, const std::string& channelName, const Client& member, bool isOperator);

	// Messages (cmd/Messaging.cpp)
	void cmdPrivmsg(Client& client, const Message& msg);
	void cmdNotice(Client& client, const Message& msg);
	void relayMessage(Client& client, const Message& msg, bool isNotice);
	void deliverMessage(const Client& sender, const std::string& command,
		const std::string& target, const std::string& text);

	// Channel operator commands (cmd/Operator.cpp)
	void cmdKick(Client& client, const Message& msg);
	void cmdInvite(Client& client, const Message& msg);
	void cmdMode(Client& client, const Message& msg);
	void channelMode(Client& client, const Message& msg);
	void userMode(Client& client, const Message& msg);

	std::string password_;
	std::string createdAt_;		// for RPL_CREATED
	std::time_t now_;			// time of the event being handled
	ClientMap clients_;
	NickMap nicks_;
	ChannelMap channels_;
	CommandMap commands_;
	std::vector<Effect> outbox_;
};

#endif
