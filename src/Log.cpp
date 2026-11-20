#include "Log.hpp"

#include <ctime>
#include <iostream>
#include <sstream>

#include <sys/stat.h>

#include "Limits.hpp"

namespace
{

std::string g_queue;			// formatted lines waiting for stderr
unsigned long g_dropped = 0;	// lines lost because the queue was full
bool g_disabled = false;		// stderr is missing or broken

std::string timestamp()
{
	char stamp[32];
	std::time_t now = std::time(NULL);
	std::tm* local = std::localtime(&now);

	if (local == NULL || std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", local) == 0)
		return "";
	return stamp;
}

bool fits(std::string::size_type size)
{
	return g_queue.size() + size <= limits::LOG_QUEUE_MAX;
}

// Queues the "N lines dropped" notice, if lines were lost and it fits.
void queueDropNotice()
{
	if (g_dropped == 0)
		return;
	std::ostringstream notice;
	notice << timestamp() << " [WARN] " << g_dropped << " log lines dropped: stderr was not keeping up\n";
	if (!fits(notice.str().size()))
		return;
	g_queue += notice.str();
	g_dropped = 0;
}

void enqueue(const char* level, const std::string& message)
{
	if (g_disabled)
		return;
	// The notice goes where the loss happened: before the next line kept.
	queueDropNotice();
	const std::string line = timestamp() + " [" + level + "] " + message + "\n";
	if (g_dropped > 0 || !fits(line.size()))
	{
		++g_dropped;
		return;
	}
	g_queue += line;
}

}

void logOpen()
{
	struct stat info;

	if (fstat(LOG_FD, &info) < 0)
		logDisable();
}

void logInfo(const std::string& message)
{
	enqueue("INFO", message);
}

void logWarn(const std::string& message)
{
	enqueue("WARN", message);
}

void logError(const std::string& message)
{
	enqueue("ERROR", message);
}

bool logPending()
{
	return !g_disabled && !g_queue.empty();
}

// One small write per POLLOUT: poll() said stderr can take data, and a
// chunk of LOG_CHUNK bytes fits in any writable pipe or terminal, so the
// write completes without waiting.
void logWriteSome()
{
	if (!logPending())
		return;
	const std::string::size_type size = (g_queue.size() < limits::LOG_CHUNK) ? g_queue.size() : limits::LOG_CHUNK;
	std::cerr.write(g_queue.data(), static_cast<std::streamsize>(size));
	std::cerr.flush();
	// A failed write cannot tell how much went out: the chunk is dropped
	// rather than written twice. A broken stderr also shows up in poll() as
	// POLLERR or POLLHUP, which disables logging.
	std::cerr.clear();
	g_queue.erase(0, size);
	if (g_queue.empty())
		queueDropNotice();
}

void logDisable()
{
	g_disabled = true;
	g_queue.clear();
	g_dropped = 0;
}
