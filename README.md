*This project has been created as part of the 42 curriculum by fguryel.*

# ft_irc

## Description

`ircserv` is an IRC server written in C++98, with no external libraries. A standard IRC client (the reference client is irssi) can connect to it, register, join channels, exchange private and channel messages, and use the channel operator commands.

The server is single-threaded and event-driven: one `poll()` call serves the listening socket and every client, all sockets are non-blocking, and nothing forks.

### Features

- **Registration:** `PASS`, `NICK`, `USER`, plus enough of `CAP` (`LS`, `REQ`, `LIST`, `END`) for modern clients that open with `CAP LS 302`.
- **Channels:** `JOIN` (with keys, and `JOIN 0` to leave every channel), `PART`, `TOPIC`, `NAMES`, `WHO`.
- **Messages:** `PRIVMSG` and `NOTICE` to nicks and channels. CTCP and DCC requests pass through unchanged, so DCC file transfer between clients works.
- **Channel operators:** `KICK`, `INVITE`, and `MODE` with `i` (invite-only), `t` (topic restricted to operators), `k` (key), `o` (operator status) and `l` (user limit). The channel creator is its first operator.
- **Connection upkeep:** `PING`/`PONG`, `QUIT`, a PING after 3 idle minutes, and timeouts for silent or never-registering clients.
- **Protection:** flood control per client, the RFC 512-byte line limit, a 2 MB send queue per client, and a 1000-client cap.
- **Networking:** IPv4 and IPv6 on one dual-stack socket.

## Instructions

### Build

```sh
make            # builds ./ircserv  (c++ -Wall -Wextra -Werror -std=c++98)
make clean      # removes object files
make fclean     # also removes the binaries
make re         # rebuilds from scratch
```

### Run

```sh
./ircserv <port> <password>
```

- `port`: the TCP port to listen on, from 1 to 65535.
- `password`: the connection password clients must send with `PASS`. It must be non-empty and contain no whitespace.

Stop the server with Ctrl+C (SIGINT) or SIGTERM. Every client receives `ERROR :Closing Link: ... (Server shutting down)`, and the server exits within 2 seconds.

### Connect

With irssi:

```sh
irssi -c 127.0.0.1 -p 6667 -w <password> -n alice
```

With netcat, one command per line (`-C` sends CRLF line endings):

```sh
nc -C 127.0.0.1 6667
PASS <password>
NICK alice
USER alice 0 * :Alice
JOIN #42
PRIVMSG #42 :hello
```

### Tests

```sh
make test          # unit tests of the IRC core: no sockets, fake clock
make integration   # builds ./ircserv, then runs tests/integration/irc_test.py (Python 3)
make asan          # builds ./ircserv_asan with AddressSanitizer and UBSan
make fuzz          # random-input testing of the core, with sanitizers

python3 tests/integration/irc_test.py ./ircserv_asan    # integration, failing on any sanitizer report
python3 tests/integration/irssi_test.py ./ircserv       # two real irssi clients (needs irssi)
python3 tests/fuzz/chaos.py ./ircserv_asan 60           # 60 s of random clients over sockets
```

What each layer covers:
- **Unit tests** (`tests/unit/`): every command, every numeric reply, and the edge cases where the right answer is a design decision.
- **Integration suite** (`tests/integration/irc_test.py`), over real sockets:
  - commands split across packets (the subject's `com` / `man` / `d` test), including 50 clients sending byte by byte
  - 200 clients in one channel, and the 1000-client cap
  - a suspended client (Ctrl+Z) receiving a flood once resumed
  - a client that never reads being dropped with "SendQ exceeded"
  - memory staying bounded under endless input without line endings
  - open file descriptors returning to their starting count after connection churn
  - forged prefixes, CR injection, binary bytes, and over-long relayed lines
  - abrupt disconnects, connection resets, and writing to a reset peer
  - IPv6, a busy port, bad arguments, and clean shutdown on SIGINT and SIGTERM
- **irssi test** (`tests/integration/irssi_test.py`): drives two real irssi clients through registration, chat, topic, every channel mode, INVITE, KICK, a nick change, `/me` and NOTICE. A logging proxy checks that irssi never receives an unexpected error and never uses a command the server does not know.
- **Fuzzers** (`tests/fuzz/`):
  - `core_fuzz` feeds random IRC-shaped traffic into the core. It checks that every reply is a single well-formed line of at most 512 bytes, and that nothing is ever sent to a closed client.
  - `chaos.py` does the same over sockets with random disconnects. It then checks that the server still answers and has no leftover connections.

To run the server under valgrind with the integration suite, point the suite at a wrapper script that starts `exec valgrind ... ./ircserv "$@"`, and set `IRC_TEST_TIME_SCALE=5` to stretch the timeouts.

## Architecture

The design is "sans-I/O": the IRC protocol and the network are two separate layers that never mix.

```
bytes from a socket ──▶ Reactor (src/net) ──▶ Core::onData(id, bytes, now)
                           ▲                          │ appends Effects
                           └──── takeEffects() ◀──────┘   SEND(id, line) / CLOSE(id, reason)
Reactor: SEND → the client's send queue → poll() reports POLLOUT → send()
         CLOSE → flush the queue, then close the socket
```

- **`src/irc/` (the core)** is the whole IRC protocol as a state machine. It makes no system calls and includes no socket headers.
  - The network layer tells it what happened through `onConnect`, `onData`, `onClose`, `onTick` and `onShutdown`.
  - It answers with a list of effects: send this line to client X, or close client X.
  - The current time is passed in, so the unit tests drive it with plain strings and a fake clock.
- **`src/net/Reactor`** owns every socket. Each loop iteration makes the program's single `poll()` call. It reads into the core, queues the core's output, writes only when `poll()` reports the socket writable, and closes connections only at the end of an iteration.
- **Client IDs** come from a counter that only goes up. Unlike file descriptors, they are never reused, so a reused fd can never receive another client's messages.

### Error handling

| Kind | How it is detected | What happens |
|---|---|---|
| Connection errors | `recv` returns 0 or -1, `send` returns -1, `POLLERR`/`POLLHUP`/`POLLNVAL` | The client is closed at the end of the iteration and its channel peers get a QUIT. `errno` is never read after `recv` or `send`. |
| Protocol errors | Command handlers throw `IrcError(numeric, ...)` from their checks (`requireChannel`, `requireMember`, `requireOperator`, ...) | `Core::dispatch` catches it and sends the numeric reply. Commands with target lists (`JOIN #a,#b`, `PRIVMSG a,b`, `KICK`, `MODE`) handle each target separately, so one bad target does not stop the others. |
| Resource limits | A line over 512 bytes, a send queue over 2 MB, out of memory | 417 reply and the line is dropped; disconnect with "SendQ exceeded"; only the client being served is dropped. |
| Startup errors | Bad arguments, port already in use | Message on stderr, exit code 1. |

### Source layout

```
src/main.cpp                 arguments, signals, wiring
src/Limits.hpp               every limit and timeout, in one place
src/Log.*                    non-blocking log on stderr (written through the poll() loop)
src/net/Reactor.*            sockets and the poll() loop
src/irc/Core.*               entry points, input framing, dispatcher, flood control, timeouts
src/irc/cmd/Registration.cpp CAP PASS NICK USER PING PONG QUIT
src/irc/cmd/Channels.cpp     JOIN PART TOPIC NAMES WHO
src/irc/cmd/Messaging.cpp    PRIVMSG NOTICE
src/irc/cmd/Operator.cpp     KICK INVITE MODE
src/irc/{Client,Channel,Message,Text,IrcError,Numerics,Effect}.*
tests/unit/                  core unit tests (make test)
tests/integration/           socket-level tests (make integration)
```

### Behaviour details

- **Password check.** `PASS`, `NICK` and `USER` may come in any order, but the password is checked as soon as `NICK` and `USER` are both in. A missing or wrong password gets 464 and an immediate disconnect.
- **Case-insensitive names.** Nick and channel names use rfc1459 case mapping: `Alice` equals `alice`, and `[a]` equals `{a}`.
- **Flood control.** Each client may send a burst of 40 lines, then 10 lines per second. While a client is over its limit, the server stops reading from its socket, so TCP slows the client down. Other clients are not affected.
- **Invites.** An invite lets its target past `+i` once. `+k` and `+l` still apply to invited clients.
- **Operators.** When the last operator leaves a channel, no one is promoted, as in standard IRC. An empty channel is deleted.
- **NOTICE** never triggers an error reply, as RFC 2812 requires.
- **Timeouts.** A client that has not finished registering after 120 seconds is disconnected. A registered client that is silent for 180 seconds receives a PING, and is disconnected if nothing arrives within 120 more seconds.

- **Logging.** Log lines go into a bounded in-memory queue (64 kB). stderr is written only when `poll()` reports it writable, 512 bytes at a time. A full pipe, a paused terminal (Ctrl+S) or a closed stderr can therefore never freeze the server. When the queue is full, lines are dropped, and a "log lines dropped" notice follows once stderr catches up.

All limits and timeouts are constants in `src/Limits.hpp`.

## Resources

- RFC 1459, Internet Relay Chat Protocol: https://www.rfc-editor.org/rfc/rfc1459
- RFC 2812, Internet Relay Chat: Client Protocol: https://www.rfc-editor.org/rfc/rfc2812
- Modern IRC Client Protocol (numerics, ISUPPORT): https://modern.ircdocs.horse/
- IRCv3 capability negotiation: https://ircv3.net/specs/extensions/capability-negotiation
- Sans-I/O protocol design: https://sans-io.readthedocs.io/
- Beej's Guide to Network Programming: https://beej.us/guide/bgnet/
- `poll(2)` manual page: https://man7.org/linux/man-pages/man2/poll.2.html

### AI usage

This project was built with Claude (Anthropic), used through Claude Code:

- **Design:** I discussed architecture alternatives with Claude: a classic reactor, a sans-I/O core behind a reactor, an actor model, and a staged pipeline. We also discussed the choice of `poll()` and the error-handling model. I made the final choices.
- **Implementation:** Claude wrote the code in `src/` and `tests/` in seven phases: skeleton, IRC core, network layer, registration, channels, operator commands, and hardening. I reviewed each phase before the next one started.
- **Testing:** Claude wrote and ran the unit tests and the integration suite, including AddressSanitizer runs.
- **Documentation:** Claude drafted this README.
