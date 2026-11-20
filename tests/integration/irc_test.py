#!/usr/bin/env python3
"""Integration tests for ircserv over real TCP sockets (Python 3 standard library only).

Usage:  python3 tests/integration/irc_test.py ./ircserv [name-filter]

Every test starts its own server on a free port with the password "pw".
Exit code 0 when every check passes.

The server path may be a wrapper script, e.g. one that runs ircserv under
valgrind. Set IRC_TEST_TIME_SCALE (default 1) to stretch every timeout for
slow builds such as valgrind (e.g. IRC_TEST_TIME_SCALE=5).
"""
import os
import pty
import re
import select
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

PASSWORD = "pw"
SERVER = None
SCALE = float(os.environ.get("IRC_TEST_TIME_SCALE", "1"))
results = []


def scaled(seconds):
    return seconds * SCALE


def proc_rss_kb(pid):
    with open("/proc/%d/status" % pid) as status:
        for line in status:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    return 0


def proc_fd_count(pid):
    return len(os.listdir("/proc/%d/fd" % pid))


def check(name, ok, detail=""):
    results.append(ok)
    print(("  PASS " if ok else "  FAIL ") + name + ("" if ok else "\n         " + repr(detail)[:600]))


def free_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


class Server:
    """Starts ircserv on a free port; stops it (SIGINT) when the test ends.

    The log (stderr) is read continuously unless read_log is False; stderr
    can also be redirected elsewhere (for example to a pseudo-terminal)."""

    def __init__(self, read_log=True, stderr=subprocess.PIPE):
        self.read_log = read_log
        self.stderr = stderr
        self.log_lines = []
        self.reader = None

    def __enter__(self):
        self.port = free_port()
        self.process = subprocess.Popen([SERVER, str(self.port), PASSWORD],
                                        stderr=self.stderr, text=True)
        if self.read_log and self.stderr == subprocess.PIPE:
            self.start_log_reader()
        deadline = time.time() + scaled(5)
        while time.time() < deadline:
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.05)
        return self

    def start_log_reader(self):
        self.reader = threading.Thread(target=self._read_log)
        self.reader.daemon = True
        self.reader.start()

    def _read_log(self):
        for line in self.process.stderr:
            self.log_lines.append(line)

    def stop(self):
        if hasattr(self, "log"):
            return self.process.returncode
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGINT)
        try:
            self.process.wait(timeout=scaled(5))
        except subprocess.TimeoutExpired:
            check("server exits within %.0f s of SIGINT" % scaled(5), False, "had to be killed")
            self.process.kill()
            self.process.wait()
        if self.reader is None and self.stderr == subprocess.PIPE:
            self.start_log_reader()
        if self.reader is not None:
            self.reader.join(timeout=scaled(5))
        self.log = "".join(self.log_lines)
        # With an ASan build (make asan), memory errors and leaks show up here.
        report = re.search(r"(AddressSanitizer|LeakSanitizer|runtime error).*", self.log)
        if report:
            check("no sanitizer report in the server log", False, report.group(0))
        return self.process.returncode

    def __exit__(self, *exc):
        self.stop()


class Client:
    def __init__(self, port, nick=None, family=socket.AF_INET, host="127.0.0.1", rcvbuf=None):
        self.sock = socket.socket(family, socket.SOCK_STREAM)
        if rcvbuf:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        self.sock.settimeout(scaled(5))
        self.sock.connect((host, port))
        self.buffer = b""
        self.closed = False
        if nick:
            self.send("PASS %s\r\nNICK %s\r\nUSER %s 0 * :%s\r\n" % (PASSWORD, nick, nick, nick))
            self.wait_for(" 422 ")

    def reset(self):
        """Closes with an RST instead of a normal FIN."""
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.sock.close()

    def send(self, data):
        self.sock.sendall(data.encode() if isinstance(data, str) else data)

    def _fill(self, deadline):
        self.sock.settimeout(max(0.01, deadline - time.time()))
        try:
            chunk = self.sock.recv(1 << 16)
        except socket.timeout:
            return False
        except ConnectionResetError:
            chunk = b""
        if not chunk:
            self.closed = True
            return False
        self.buffer += chunk
        return True

    def _pop_line(self):
        if b"\r\n" not in self.buffer:
            return None
        line, self.buffer = self.buffer.split(b"\r\n", 1)
        # surrogateescape keeps every byte: line.encode("utf-8", "surrogateescape")
        # gives back exactly what the server sent.
        return line.decode("utf-8", "surrogateescape")

    def lines(self, wait=0.4):
        """Every line that arrives within wait seconds."""
        deadline, out = time.time() + scaled(wait), []
        while True:
            line = self._pop_line()
            if line is not None:
                out.append(line)
                continue
            if time.time() >= deadline or not self._fill(deadline):
                return out

    def wait_for(self, needle, timeout=5.0):
        """Reads until a line contains needle; returns it, or None on timeout/close."""
        deadline = time.time() + scaled(timeout)
        while True:
            line = self._pop_line()
            if line is not None:
                if needle in line:
                    return line
                continue
            if time.time() >= deadline or not self._fill(deadline):
                return None

    def read_until_closed(self, timeout=5.0):
        deadline = time.time() + scaled(timeout)
        while not self.closed and time.time() < deadline:
            self._fill(deadline)
        return self.buffer.decode("utf-8", "surrogateescape"), self.closed

    def close(self):
        self.sock.close()


def error_numerics(lines):
    return [l for l in lines if re.match(r"^:\S+ [45]\d\d ", l) and " 422 " not in l]


# ---------------------------------------------------------------- tests

def test_partial_command():
    """The subject's test: 'com', 'man', 'd\\n' sent separately (nc + Ctrl+D)."""
    with Server() as srv:
        c = Client(srv.port)
        for part in (b"com", b"man", b"d\n"):
            c.send(part)
            time.sleep(0.2)
        check("pieces reassembled into one command", c.lines() == [":ircserv 451 * :You have not registered"])
        a, b = Client(srv.port, "alice"), Client(srv.port, "bob")
        b.send("JOIN #r\r\n")
        a.send("JOIN #r\r\n")
        b.wait_for(" JOIN #r")
        for part in ("PRIV", "MSG #r :hel", "lo\r\n"):
            a.send(part)
            time.sleep(0.1)
        check("split PRIVMSG delivered whole", b.wait_for("PRIVMSG") == ":alice!alice@127.0.0.1 PRIVMSG #r :hello")


def test_irssi_session_has_no_errors():
    """The exact sequence irssi sends on connect and on /join."""
    with Server() as srv:
        c = Client(srv.port)
        c.send("CAP LS 302\r\nPASS pw\r\nNICK alice\r\nUSER alice alice 127.0.0.1 :Alice\r\n")
        check("CAP LS holds registration", c.lines() == [":ircserv CAP * LS :"])
        c.send("CAP END\r\nMODE alice +i\r\nJOIN #room\r\nMODE #room\r\nWHO #room\r\nMODE #room b\r\n")
        got = c.lines(0.6)
        check("welcome burst received", any(" 001 alice " in l for l in got), got)
        check("no error numerics anywhere", error_numerics(got) == [], error_numerics(got))


def test_bad_password():
    with Server() as srv:
        c = Client(srv.port)
        c.send("PASS wrong\r\nNICK x\r\nUSER x 0 * :x\r\n")
        data, closed = c.read_until_closed()
        check("464 then ERROR then close",
              data == ":ircserv 464 * :Password incorrect\r\nERROR :Closing Link: 127.0.0.1 (Bad password)\r\n"
              and closed, (data, closed))
        c = Client(srv.port)
        c.send("NICK x\r\nUSER x 0 * :x\r\n")
        data, closed = c.read_until_closed()
        check("missing PASS refused at registration", "464" in data and closed, data)


def test_channel_and_operator_commands():
    with Server() as srv:
        alice, bob, carol = Client(srv.port, "alice"), Client(srv.port, "bob"), Client(srv.port, "carol")
        alice.send("JOIN #room\r\n")
        check("creator is operator", alice.wait_for(" 353 ") == ":ircserv 353 alice = #room :@alice")
        bob.send("JOIN #room\r\n")
        alice.wait_for("JOIN")
        bob.lines()
        alice.send("PRIVMSG #room :hi all\r\n")
        check("channel message reaches member", bob.wait_for("PRIVMSG") == ":alice!alice@127.0.0.1 PRIVMSG #room :hi all")
        bob.send("PRIVMSG alice :psst\r\n")
        check("private message", alice.wait_for("PRIVMSG") == ":bob!bob@127.0.0.1 PRIVMSG alice :psst")

        alice.send("MODE #room +itkl key 10\r\n")
        check("mode change broadcast", bob.wait_for("MODE") == ":alice!alice@127.0.0.1 MODE #room +itkl key 10")
        bob.send("TOPIC #room :mine\r\n")
        check("+t blocks non-operator", bob.wait_for(" 482 ") is not None)
        carol.send("JOIN #room key\r\n")
        check("+i blocks uninvited", carol.wait_for(" 473 ") is not None)
        alice.send("INVITE carol #room\r\n")
        check("INVITE reaches target", carol.wait_for("INVITE") == ":alice!alice@127.0.0.1 INVITE carol #room")
        carol.send("JOIN #room nope\r\n")
        check("+k still applies to invited", carol.wait_for(" 475 ") is not None)
        carol.send("JOIN #room key\r\n")
        check("invited client joins with key", carol.wait_for(" 366 ") is not None)

        alice.send("MODE #room +o bob\r\n")
        bob.wait_for("MODE #room +o bob")
        bob.send("KICK #room carol :out\r\n")
        check("KICK by new operator", carol.wait_for("KICK") == ":bob!bob@127.0.0.1 KICK #room carol :out")
        carol.send("PRIVMSG #room :back?\r\n")
        check("kicked client cannot speak", carol.wait_for(" 404 ") is not None)


def test_many_clients_fan_out():
    with Server() as srv:
        count = 200
        clients = [Client(srv.port, "u%03d" % i) for i in range(count)]
        for c in clients:
            c.send("JOIN #load\r\n")
        for c in clients:
            c.wait_for(" 366 ", timeout=10)
        clients[0].send("PRIVMSG #load :fan-out\r\n")
        received = sum(1 for c in clients[1:] if c.wait_for(":fan-out", timeout=10))
        check("%d clients all receive one channel message" % (count - 1), received == count - 1, received)


def test_suspended_reader_gets_everything():
    """Evaluation scenario: Ctrl+Z one client, flood the channel, resume it."""
    with Server() as srv:
        alice, bob, carol = Client(srv.port, "alice"), Client(srv.port, "bob"), Client(srv.port, "carol")
        alice.send("JOIN #room\r\n")
        alice.wait_for(" 366 ")
        bob.send("JOIN #room\r\n")
        bob.wait_for(" 366 ")
        alice.lines()
        total = 100
        alice.send("".join("PRIVMSG #room :flood %03d\r\n" % i for i in range(total)))
        start = time.time()
        carol.send("PING :alive\r\n")
        check("others served during the flood",
              carol.wait_for("PONG") is not None and time.time() - start < scaled(1))
        time.sleep(8)  # bob is "suspended": reads nothing
        got = [l for l in bob.lines(3) if "PRIVMSG" in l]
        expected = [":alice!alice@127.0.0.1 PRIVMSG #room :flood %03d" % i for i in range(total)]
        check("resumed client gets every message in order", got == expected, (len(got), got[-1:]))


def test_sendq_overflow_drops_only_the_slow_client():
    """A client that never reads is dropped once 2 MB wait for it; nobody else is."""
    with Server() as srv:
        observer = Client(srv.port, "observer")
        victim = Client(srv.port, "victim", rcvbuf=4096)
        observer.send("JOIN #watch\r\n")
        observer.wait_for(" 366 ")
        victim.send("JOIN #watch\r\n")
        victim.wait_for(" 366 ")
        observer.lines()

        senders = [Client(srv.port, "s%03d" % i) for i in range(200)]
        payload = "".join("PRIVMSG victim :%s\r\n" % ("x" * 400) for _ in range(100)).encode()
        for s in senders:
            s.sock.setblocking(False)
            try:
                s.sock.send(payload)
            except BlockingIOError:
                pass
        quit_line = observer.wait_for("QUIT", timeout=40)
        check("slow client dropped with SendQ exceeded",
              quit_line == ":victim!victim@127.0.0.1 QUIT :SendQ exceeded", quit_line)
        start = time.time()
        observer.send("PING :ok\r\n")
        check("server still responsive", observer.wait_for("PONG", timeout=5) is not None
              and time.time() - start < scaled(2))
        srv.stop()
        check("drop logged", "disconnected: SendQ exceeded" in srv.log)


def test_abrupt_disconnects():
    with Server() as srv:
        alice, bob = Client(srv.port, "alice"), Client(srv.port, "bob")
        alice.send("JOIN #r\r\n")
        alice.wait_for(" 366 ")
        bob.send("JOIN #r\r\n")
        alice.wait_for("JOIN")
        # Read everything first: closing with unread input makes the kernel
        # send a reset instead of a normal close (that case is tested below).
        bob.wait_for(" 366 ")
        bob.send("PRIVMSG #r :half a comm")
        time.sleep(0.1)
        bob.close()
        quit_line = alice.wait_for("QUIT")
        check("peer sees QUIT after a client vanishes mid-command",
              quit_line == ":bob!bob@127.0.0.1 QUIT :Connection closed", quit_line)
        reset = Client(srv.port, "carol")
        reset.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        reset.close()
        time.sleep(0.2)
        alice.send("PING :x\r\n")
        check("server alive after a connection reset", alice.wait_for("PONG") is not None
              and srv.process.poll() is None)


def test_line_limits_and_flood_control():
    with Server() as srv:
        c = Client(srv.port)
        c.send(b"A" * 600 + b"\r\nNEXT\r\n")
        check("oversized line gets 417, next line still works",
              c.lines() == [":ircserv 417 * :Input line was too long", ":ircserv 451 * :You have not registered"])
        # A fresh client has a full bucket of 40 tokens. The bucket refills
        # by 10 each second, so a second boundary inside the window may add 10.
        flooder = Client(srv.port)
        flooder.send(b"FOO\r\n" * 80)
        first = len(flooder.lines(0.5))
        check("burst limited to about 40 lines", 40 <= first <= (50 if SCALE == 1 else 79), first)
        rest = len(flooder.lines(6))
        check("all 80 lines answered, throttled", first + rest == 80, (first, rest))


def test_ipv6_and_port_in_use():
    with Server() as srv:
        try:
            c = Client(srv.port, "six", family=socket.AF_INET6, host="::1")
            c.send("WHO six\r\n")
            check("IPv6 client registered, host shown as 0::1", " 0::1 " in (c.wait_for(" 352 ") or ""))
        except OSError as error:
            check("IPv6 client registered", False, error)
        second = subprocess.run([SERVER, str(srv.port), PASSWORD], capture_output=True, text=True, timeout=5)
        check("second server on a busy port exits 1",
              second.returncode == 1 and "cannot listen" in second.stderr, second.stderr)


def test_bad_arguments():
    for args in ([], ["6667"], ["0", "pw"], ["65536", "pw"], ["66a", "pw"], ["6667", ""], ["6667", "a b"]):
        result = subprocess.run([SERVER] + args, capture_output=True, text=True, timeout=5)
        check("rejects arguments %r" % args, result.returncode == 1 and result.stderr, result)


def test_signal_shutdown():
    for signum in (signal.SIGINT, signal.SIGTERM):
        name = signal.Signals(signum).name
        with Server() as srv:
            a, b = Client(srv.port, "alice"), Client(srv.port)
            a.send("JOIN #r\r\n")
            a.wait_for(" 366 ")
            srv.process.send_signal(signum)
            start = time.time()
            for c in (a, b):
                data, closed = c.read_until_closed()
                check("%s: client gets ERROR then close" % name,
                      data.endswith("ERROR :Closing Link: 127.0.0.1 (Server shutting down)\r\n") and closed, data)
            check("%s: exit code 0" % name, srv.stop() == 0)
            check("%s: stopped within 3 s" % name, time.time() - start < scaled(3))


# ---------------------------------------------------------------- edge cases

def test_client_limit():
    """The 1001st simultaneous connection is closed at once; the rest keep working."""
    with Server() as srv:
        conns = [socket.create_connection(("127.0.0.1", srv.port), timeout=scaled(5)) for _ in range(1000)]
        extra = socket.create_connection(("127.0.0.1", srv.port), timeout=scaled(15))
        try:
            refused = extra.recv(16) == b""
        except (socket.timeout, ConnectionResetError) as error:
            refused = isinstance(error, ConnectionResetError)
        check("connection 1001 is closed immediately", refused)
        conns[0].sendall(b"PING :still-here\r\n")
        conns[0].settimeout(scaled(5))
        check("an accepted connection is still served", b"PONG" in conns[0].recv(512))
        conns[1].close()
        time.sleep(scaled(0.5))
        late = Client(srv.port)
        late.send("PING :late\r\n")
        check("a freed slot accepts a new client", late.wait_for("PONG") is not None)
        for c in conns:
            c.close()
        srv.stop()
        check("refusal logged", "server full" in srv.log)


def test_memory_bounded_without_newlines():
    """Endless input with no line ending is discarded, not buffered."""
    with Server() as srv:
        clients = [Client(srv.port) for _ in range(10)]
        before = proc_rss_kb(srv.process.pid)
        chunk = b"A" * 65536
        for _ in range(32):  # 2 MB per client, 20 MB in total
            for c in clients:
                c.sock.sendall(chunk)
        for c in clients:
            c.send("\r\nPING :done\r\n")
        replies = [c.lines(2) for c in clients]
        check("each client gets exactly one 417, then normal service",
              all(r == [":ircserv 417 * :Input line was too long", ":ircserv PONG ircserv :done"] for r in replies),
              replies[0])
        growth = proc_rss_kb(srv.process.pid) - before
        if "asan" in SERVER or SCALE > 1:
            print("  INFO server RSS grew %d kB (not checked for sanitizer/valgrind builds)" % growth)
        else:
            check("server memory grew less than 5 MB for 20 MB of input", growth < 5 * 1024, growth)


def test_no_fd_leak_after_churn():
    with Server() as srv:
        steady = Client(srv.port, "steady")
        steady.send("JOIN #churn\r\n")
        steady.wait_for(" 366 ")
        baseline = proc_fd_count(srv.process.pid)
        for i in range(100):
            kind = i % 5
            if kind == 0:
                c = Client(srv.port, "c%d" % i)
                c.send("JOIN #churn\r\nQUIT :bye\r\n")
                c.read_until_closed()
            elif kind == 1:
                Client(srv.port).close()
            elif kind == 2:
                Client(srv.port).reset()
            elif kind == 3:
                c = Client(srv.port)
                c.send("PASS nope\r\n")
                c.read_until_closed()
            else:
                c = Client(srv.port, "d%d" % i)
                c.send("JOIN #churn\r\nPRIVMSG #churn :half")
                c.wait_for(" 366 ")
                c.close()
        deadline = time.time() + scaled(5)
        while proc_fd_count(srv.process.pid) != baseline and time.time() < deadline:
            time.sleep(0.1)
        after = proc_fd_count(srv.process.pid)
        check("open fds back to baseline after 100 connect/disconnect cycles", after == baseline, (baseline, after))
        steady.send("PING :ok\r\n")
        check("steady client still served", steady.wait_for("PONG") is not None)


def test_message_integrity():
    with Server() as srv:
        alice, bob = Client(srv.port, "alice"), Client(srv.port, "bob")

        alice.send(":admin!root@server PRIVMSG bob :spoof?\r\n")
        check("a client-supplied prefix is ignored",
              bob.wait_for("spoof?") == ":alice!alice@127.0.0.1 PRIVMSG bob :spoof?")

        alice.send("PRIVMSG bob :hi\r:ircserv 001 bob :fake welcome\r\n")
        got = bob.lines(0.5)
        check("a CR cannot inject a line into another client's stream",
              got == [":alice!alice@127.0.0.1 PRIVMSG bob :hi"], got)
        check("the injected part is the sender's own (unknown) command",
              alice.wait_for(" 421 ") == ":ircserv 421 alice 001 :Unknown command")

        payload = bytes(b for b in range(256) if b not in (10, 13))
        alice.send(b"PRIVMSG bob :" + payload + b"\r\n")
        line = bob.wait_for("PRIVMSG")
        raw = line.encode("utf-8", "surrogateescape") if line else b""
        check("all byte values but CR/LF relayed unchanged (NUL, CTCP, UTF-8, high bytes)",
              raw == b":alice!alice@127.0.0.1 PRIVMSG bob :" + payload, raw[:80])

        alice.send("PRIVMSG bob :" + "x" * 497 + "\r\n")   # a full 510-byte line
        line = bob.wait_for("PRIVMSG") or ""
        check("a relayed line that grows past 512 bytes is cut to 510 + CRLF",
              len(line) == 510 and line.startswith(":alice!alice@127.0.0.1 PRIVMSG bob :xxx"), len(line))


def test_interleaved_byte_by_byte_clients():
    """50 clients each send one message one byte at a time, interleaved."""
    with Server() as srv:
        observer = Client(srv.port, "observer")
        observer.send("JOIN #mix\r\n")
        observer.wait_for(" 366 ")
        senders = [Client(srv.port, "m%02d" % i) for i in range(50)]
        for s in senders:
            s.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.send("JOIN #mix\r\n")
            s.wait_for(" 366 ")
        observer.lines(0.5)
        messages = [("PRIVMSG #mix :hello from m%02d\r\n" % i).encode() for i in range(50)]
        for position in range(max(len(m) for m in messages)):
            for s, m in zip(senders, messages):
                if position < len(m):
                    s.sock.sendall(m[position:position + 1])
        got = sorted(l for l in observer.lines(3) if "PRIVMSG" in l)
        expected = sorted(":m%02d!m%02d@127.0.0.1 PRIVMSG #mix :hello from m%02d" % (i, i, i) for i in range(50))
        check("every message reassembled intact", got == expected, (len(got), got[:2]))


def test_peer_reset_with_pending_output():
    """Writing to a connection the peer has reset must not kill the server (SIGPIPE)."""
    with Server() as srv:
        alice = Client(srv.port, "alice", rcvbuf=4096)
        alice.send("JOIN #big\r\n")
        alice.wait_for(" 366 ")
        helpers = [Client(srv.port, "h%02d" % i) for i in range(30)]
        for h in helpers:
            h.send("JOIN #big\r\n")
        for h in helpers:
            h.send("PRIVMSG #big :%s\r\n" % ("y" * 400))
        time.sleep(scaled(0.3))
        alice.reset()
        time.sleep(scaled(0.5))
        helpers[0].send("PING :alive\r\n")
        check("server survives writing to a reset connection",
              helpers[0].wait_for("PONG") is not None and srv.process.poll() is None)
        check("peers see the reset client quit", helpers[1].wait_for("QUIT") is not None)


def churn(port, count):
    """Opens and closes connections: two log lines each."""
    for _ in range(count):
        Client(port).close()


def test_unread_log_pipe_never_blocks_server():
    """stderr is a pipe nobody reads: once it is full, logging must not stall the server."""
    with Server(read_log=False) as srv:
        watcher = Client(srv.port, "watcher")
        churn(srv.port, 1500)  # about 3000 log lines, far more than a 64 kB pipe holds
        # Connections are accepted in order: once this one is registered, the
        # server has handled (and logged) every connection before it.
        Client(srv.port, "last", rcvbuf=None)
        start = time.time()
        watcher.send("PING :alive\r\n")
        check("server answers while its log pipe is full",
              watcher.wait_for("PONG", timeout=5) is not None and time.time() - start < scaled(2))
        srv.start_log_reader()
        time.sleep(scaled(1))
        check("exit code 0 with a stalled log", srv.stop() == 0)
        check("dropped log lines are reported once the pipe drains", "log lines dropped" in srv.log)


def test_paused_terminal_never_blocks_server():
    """stderr is a terminal paused with Ctrl+S: logging must not stall the server."""
    master, slave = pty.openpty()
    with Server(stderr=slave) as srv:
        os.close(slave)
        os.write(master, b"\x13")  # Ctrl+S: the terminal stops all output
        watcher = Client(srv.port, "watcher")
        churn(srv.port, 300)
        start = time.time()
        watcher.send("PING :alive\r\n")
        check("server answers while its terminal is paused",
              watcher.wait_for("PONG", timeout=5) is not None and time.time() - start < scaled(2))
        os.write(master, b"\x11")  # Ctrl+Q: output resumes
        output, deadline = b"", time.time() + scaled(3)
        while time.time() < deadline:
            try:
                ready = select.select([master], [], [], 0.2)[0]
                if ready:
                    output += os.read(master, 65536)
            except OSError:
                break
        check("the log flows again after Ctrl+Q", b"connected from" in output, output[-200:])
        check("exit code 0", srv.stop() == 0)
    os.close(master)


def test_closed_stderr_is_tolerated():
    """Started with stderr closed (2>&-): the server must still work normally."""
    port = free_port()
    process = subprocess.Popen(["sh", "-c", 'exec "$0" "$1" pw 2>&-', SERVER, str(port)])
    try:
        deadline = time.time() + scaled(5)
        client = None
        while client is None and time.time() < deadline:
            try:
                client = Client(port)
            except OSError:
                time.sleep(0.1)
        check("server accepts connections with stderr closed", client is not None)
        if client is not None:
            churn(port, 50)
            client.send("PASS pw\r\nNICK alice\r\nUSER alice 0 * :A\r\nPING :x\r\n")
            check("registers and answers with stderr closed",
                  client.wait_for(" 001 alice ") is not None and client.wait_for("PONG") is not None)
    finally:
        process.send_signal(signal.SIGINT)
        try:
            code = process.wait(timeout=scaled(5))
        except subprocess.TimeoutExpired:
            process.kill()
            code = "killed"
    check("exit code 0 with stderr closed", code == 0, code)


def test_privileged_port_refused():
    if os.geteuid() == 0:
        print("  SKIP running as root: port 80 is allowed")
        return
    probe = socket.socket()
    try:
        probe.bind(("0.0.0.0", 80))
        print("  SKIP this system lets unprivileged users bind port 80")
        return
    except OSError:
        pass
    finally:
        probe.close()
    result = subprocess.run([SERVER, "80", PASSWORD], capture_output=True, text=True, timeout=scaled(10))
    check("binding port 80 without root exits 1 with the reason",
          result.returncode == 1 and "Permission denied" in result.stderr, result.stderr)


def main():
    global SERVER
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    SERVER = sys.argv[1]
    pattern = sys.argv[2] if len(sys.argv) > 2 else ""
    tests = [(name, fn) for name, fn in sorted(globals().items())
             if name.startswith("test_") and pattern in name]
    for name, fn in tests:
        print(name)
        fn()
    failed = results.count(False)
    print("\n%d checks, %d failed" % (len(results), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
