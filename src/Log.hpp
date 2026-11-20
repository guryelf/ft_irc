#ifndef LOG_HPP
#define LOG_HPP

#include <string>

// Timestamped logging on stderr that never blocks the event loop.
//
// logInfo/logWarn/logError only append a line to a bounded in-memory queue.
// The Reactor puts stderr in its poll() set while lines are waiting, and on
// POLLOUT writes one small chunk with logWriteSome(). A full pipe or a paused
// terminal (Ctrl+S) therefore delays the log, never the server. When the
// queue is full, new lines are dropped and counted; a notice reports how many
// once stderr catches up.
//
// Only main and the network shell log: the IRC core performs no I/O.
// Never pass secrets (such as the connection password) to these functions.

const int LOG_FD = 2;

// Call first in main, before any socket exists: if stderr is closed
// (started with 2>&-), logging stays off, and fd 2 is never written even
// after a socket reuses that number.
void logOpen();

void logInfo(const std::string& message);
void logWarn(const std::string& message);
void logError(const std::string& message);

// For the Reactor's poll() loop.
bool logPending();		// queued output waits for stderr
void logWriteSome();	// poll() reported POLLOUT on LOG_FD
void logDisable();		// stderr is broken (POLLERR/POLLHUP/POLLNVAL): stop logging

#endif
