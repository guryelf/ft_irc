#ifndef LIMITS_HPP
#define LIMITS_HPP

#include <cstddef>
#include <ctime>

// Every server tunable lives here as a compile-time constant.
// Values are fixed on purpose: none of them needs a configuration file.
namespace limits
{

// Identity
const char* const SERVER_NAME = "ircserv";
const char* const SERVER_VERSION = "ircserv-1.0";

// Protocol (RFC 2812 limits, plus the values advertised in RPL_ISUPPORT)
const std::size_t MAX_LINE = 512;			// one message, CRLF included
const std::size_t MAX_PARAMS = 15;
const std::size_t NICKLEN = 30;
const std::size_t USERLEN = 10;				// longer user names are cut
const std::size_t CHANNELLEN = 50;
const std::size_t TOPICLEN = 390;
const std::size_t CHANLIMIT = 20;			// channels one client may join

// Network shell
const std::size_t RECV_CHUNK = 4096;		// bytes read per readiness event
const std::size_t SENDQ_MAX = 2 * 1024 * 1024;	// unsent bytes before "SendQ exceeded"
const std::size_t MAX_CLIENTS = 1000;		// stays below the usual 1024 fd limit
const int POLL_TIMEOUT_MS = 1000;			// also the tick period for timeouts
const std::time_t CLOSE_FLUSH_SECONDS = 5;	// time to flush ERROR before closing
const std::time_t SHUTDOWN_FLUSH_SECONDS = 2;

// Logging: stderr is written only when poll() reports it writable
const std::size_t LOG_QUEUE_MAX = 64 * 1024;	// queued log bytes before lines are dropped
const std::size_t LOG_CHUNK = 512;			// bytes per write: fits any writable pipe (PIPE_BUF >= 512)

// Liveness
const std::time_t REG_TIMEOUT = 120;		// seconds to finish PASS/NICK/USER
const std::time_t PING_IDLE = 180;			// idle seconds before the server sends PING
const std::time_t PING_TIMEOUT = 120;		// seconds to answer that PING

// Flood control (token bucket per client)
const unsigned FLOOD_RATE = 10;				// lines per second, sustained
const unsigned FLOOD_BURST = 40;			// lines accepted at once

}

#endif
