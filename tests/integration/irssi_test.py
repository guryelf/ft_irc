#!/usr/bin/env python3
"""Drives two real irssi clients (the reference client) against ircserv.

Usage:  python3 tests/integration/irssi_test.py ./ircserv

irssi runs in pseudo-terminals and receives typed commands. It connects
through a small logging proxy, so every line it sends and receives is
recorded. The test checks that:
  - irssi never receives an error numeric, except the one the scenario
    triggers on purpose (473 when joining an invite-only channel)
  - irssi never sends a command the server does not know (no 421)
  - every step of the scenario reaches the other client, and irssi shows it
    in its window logs
Set IRC_TEST_TIME_SCALE (default 1) to slow the scenario down for valgrind.
Set IRC_TEST_TRANSCRIPT=<file> to save every proxied line, for debugging.
"""
import fcntl
import glob
import os
import pty
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
from collections import Counter

PASSWORD = "pw"
SCALE = float(os.environ.get("IRC_TEST_TIME_SCALE", "1"))
results = []


def check(name, ok, detail=""):
    results.append(ok)
    print(("  PASS " if ok else "  FAIL ") + name + ("" if ok else "\n         " + repr(detail)[:800]))


def free_port():
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    return port


class Proxy:
    """Forwards TCP connections to the server and records every line both ways."""

    def __init__(self, upstream_port):
        self.upstream_port = upstream_port
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(8)
        self.port = self.listener.getsockname()[1]
        self.records = []	# (connection index, "C" or "S", line)
        self.lock = threading.Lock()
        self.connections = 0
        thread = threading.Thread(target=self._accept)
        thread.daemon = True
        thread.start()

    def _accept(self):
        while True:
            try:
                client, _ = self.listener.accept()
            except OSError:
                return
            upstream = socket.create_connection(("127.0.0.1", self.upstream_port))
            index = self.connections
            self.connections += 1
            for src, dst, direction in ((client, upstream, "C"), (upstream, client, "S")):
                thread = threading.Thread(target=self._pump, args=(src, dst, index, direction))
                thread.daemon = True
                thread.start()

    def _pump(self, src, dst, index, direction):
        pending = b""
        while True:
            try:
                data = src.recv(65536)
            except OSError:
                data = b""
            if not data:
                try:
                    dst.shutdown(socket.SHUT_WR)
                except OSError:
                    pass
                return
            try:
                dst.sendall(data)
            except OSError:
                return
            pending += data
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                with self.lock:
                    self.records.append((index, direction, line.rstrip(b"\r").decode("utf-8", "replace")))

    def lines(self, nick, direction):
        """Lines of the connection whose client registered as nick."""
        with self.lock:
            records = list(self.records)
        owner = None
        for index, way, line in records:
            if way == "C" and line.upper().startswith("NICK ") and line.split()[1].lstrip(":") == nick:
                owner = index
                break
        return [line for index, way, line in records if index == owner and way == direction]


class Irssi:
    """One irssi process in a pseudo-terminal, with its own home directory."""

    def __init__(self, nick, home):
        self.nick = nick
        self.home = home
        os.makedirs(os.path.join(home, "logs"))
        # irssi queues commands (5 at once, then one per 2.2 s) as its own
        # flood protection; the test turns that off so steps happen in order.
        with open(os.path.join(home, "config"), "w") as config:
            config.write(
                'settings = {\n'
                '  core = { real_name = "%s test"; user_name = "%s"; nick = "%s"; };\n'
                '  "irc/core" = { cmd_queue_speed = "0"; cmds_max_at_once = "100"; };\n'
                '  "fe-common/core" = { autolog = "yes"; autolog_path = "%s/logs/$tag/$0.log"; };\n'
                '};\n' % (nick, nick, nick, home))
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 200, 0, 0))
        self.process = subprocess.Popen(["irssi", "--home=" + home], stdin=slave, stdout=slave, stderr=slave,
                                        env=dict(os.environ, TERM="xterm"), start_new_session=True)
        os.close(slave)
        self.master = master
        # irssi must always be able to write to its terminal: drain it.
        thread = threading.Thread(target=self._drain)
        thread.daemon = True
        thread.start()

    def _drain(self):
        while True:
            try:
                if not os.read(self.master, 65536):
                    return
            except OSError:
                return

    def type(self, text):
        os.write(self.master, text.encode() + b"\r")

    def window_log(self):
        text = ""
        for path in glob.glob(os.path.join(self.home, "logs", "*", "*.log")):
            with open(path, errors="replace") as log:
                text += log.read()
        return text

    def wait_exit(self, timeout):
        try:
            return self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            return None


def has(lines, pattern):
    return any(re.search(pattern, line) for line in lines)


def step(proxy, client, text, observer, pattern, name):
    """Types text into one irssi, then waits until observer receives a line
    matching pattern: each step is checked by its effect, not by a delay."""
    seen = len(proxy.lines(observer, "S"))
    client.type(text)
    deadline = time.time() + 8 * SCALE
    while time.time() < deadline:
        if has(proxy.lines(observer, "S")[seen:], pattern):
            check(name, True)
            return True
        time.sleep(0.05)
    check(name, False, "typed %r; %s never received /%s/" % (text, observer, pattern))
    return False


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if shutil.which("irssi") is None:
        print("irssi is not installed")
        return 2
    server_port = free_port()
    server = subprocess.Popen([sys.argv[1], str(server_port), PASSWORD], stderr=subprocess.PIPE, text=True)
    server_log = []
    log_reader = threading.Thread(target=lambda: [server_log.append(l) for l in server.stderr])
    log_reader.daemon = True
    log_reader.start()
    time.sleep(1.0 * SCALE)
    proxy = Proxy(server_port)
    workdir = tempfile.mkdtemp(prefix="irssi-test-")
    alice = Irssi("alice", os.path.join(workdir, "alice"))
    bob = Irssi("bob", os.path.join(workdir, "bob"))
    time.sleep(2 * SCALE)
    try:
        connect = "/connect 127.0.0.1 %d %s %%s" % (proxy.port, PASSWORD)
        mask = r"!\S+@127\.0\.0\.1 "
        steps = [
            (alice, connect % "alice", "alice", r" 001 alice ", "alice registers"),
            (bob, connect % "bob", "bob", r" 001 bob ", "bob registers"),
            (alice, "/join #42", "alice", r" 366 alice #42 ", "alice creates #42"),
            (bob, "/join #42", "alice", r"^:bob" + mask + "JOIN #42$", "bob joins, alice sees it"),
            (alice, "hello from alice", "bob", r"^:alice" + mask + "PRIVMSG #42 :hello from alice$", "channel message"),
            (bob, "/msg alice psst private", "alice", r"^:bob" + mask + "PRIVMSG alice :psst private$", "private message"),
            (alice, "/topic #42 Weekly sync", "bob", r"TOPIC #42 :Weekly sync$", "topic change"),
            (alice, "/mode #42 +i", "bob", r"^:alice" + mask + r"MODE #42 \+i$", "+i"),
            (bob, "/part #42 brb", "alice", r"^:bob" + mask + "PART #42 :brb$", "bob parts"),
            (bob, "/join #42", "bob", r" 473 bob #42 ", "+i refuses bob (deliberate 473)"),
            (alice, "/invite bob #42", "bob", r"^:alice" + mask + "INVITE bob #42$", "invite"),
            (bob, "/join #42", "alice", r"^:bob" + mask + "JOIN #42$", "invited bob joins"),
            (alice, "/mode #42 +k secret", "bob", r"MODE #42 \+k secret$", "+k"),
            (alice, "/mode #42 +l 5", "bob", r"MODE #42 \+l 5$", "+l"),
            (alice, "/mode #42 +o bob", "bob", r"MODE #42 \+o bob$", "+o bob"),
            (bob, "/mode #42 -o alice", "alice", r"^:bob" + mask + "MODE #42 -o alice$", "bob deops alice"),
            (bob, "/kick #42 alice bye alice", "alice", r"^:bob" + mask + "KICK #42 alice :bye alice$", "kick"),
            (bob, "/mode #42 -i", "bob", r"MODE #42 -i$", "-i"),
            (alice, "/join #42", "alice", r" 475 alice #42 ", "+k refuses alice without key (deliberate 475)"),
            (alice, "/join #42 secret", "bob", r"^:alice" + mask + "JOIN #42$", "alice rejoins with the key"),
            (alice, "/nick alice2", "bob", r"^:alice" + mask + "NICK :alice2$", "nick change"),
            (alice, "/me waves", "bob", r"PRIVMSG #42 :\x01ACTION waves\x01$", "/me as CTCP ACTION"),
            (bob, "/notice alice2 a notice", "alice", r"NOTICE alice2 :a notice$", "notice"),
            # irssi answers /names and a bare /topic from its own cache, so
            # /quote sends the raw commands to reach the server.
            (bob, "/quote NAMES #42", "bob", r" 353 bob = #42 :@bob alice2$| 353 bob = #42 :alice2 @bob$", "names"),
            (bob, "/quote TOPIC #42", "bob", r" 332 bob #42 :Weekly sync$", "topic query"),
            (alice, "/quit see you", "bob", r"^:alice2" + mask + "QUIT :Quit: see you$", "alice quits"),
        ]
        for client, text, observer, pattern, name in steps:
            if not step(proxy, client, text, observer, pattern, name) and "registers" in name:
                break
        bob.type("/quit done")
        check("both irssi clients exit after /quit",
              alice.wait_exit(10 * SCALE) is not None and bob.wait_exit(10 * SCALE) is not None)
    finally:
        for client in (alice, bob):
            if client.process.poll() is None:
                client.process.kill()

    transcript = os.environ.get("IRC_TEST_TRANSCRIPT")
    if transcript:
        with open(transcript, "w") as out:
            for index, direction, line in proxy.records:
                out.write("%d %s %s\n" % (index, "->" if direction == "C" else "<-", line))
    to_alice, to_bob = proxy.lines("alice", "S"), proxy.lines("bob", "S")
    from_alice, from_bob = proxy.lines("alice", "C"), proxy.lines("bob", "C")

    def unexpected_errors(lines, deliberate):
        # irssi sends "JOIN :" right after "CAP LS 302" to probe for CAP
        # support; like real servers, ircserv answers it with 451 before
        # registration. Errors the scenario causes on purpose are listed too.
        errors = []
        registered = False
        for line in lines:
            registered = registered or " 001 " in line
            if re.match(r"^:\S+ [45]\d\d ", line) and " 422 " not in line:
                if not registered and " 451 " in line:
                    continue
                if any(code in line for code in deliberate):
                    continue
                errors.append(line)
        return errors

    print("irssi sent: " + ", ".join("%s x%d" % kv for kv in
                                     sorted(Counter(l.split()[0].upper() for l in from_alice + from_bob if l).items())))
    check("no 421: irssi only uses commands the server knows",
          not has(to_alice + to_bob, r"^:\S+ 421 "), [l for l in to_alice + to_bob if " 421 " in l])
    check("alice got no error numerics beyond the deliberate 475",
          unexpected_errors(to_alice, [" 475 alice #42 "]) == [], unexpected_errors(to_alice, [" 475 alice #42 "]))
    check("bob got no error numerics beyond the deliberate 473",
          unexpected_errors(to_bob, [" 473 bob #42 "]) == [], unexpected_errors(to_bob, [" 473 bob #42 "]))

    check("irssi displayed the channel message (bob's window log)", "hello from alice" in bob.window_log())
    check("irssi displayed the private message (alice's window log)", "psst private" in alice.window_log())
    check("irssi displayed the kick (alice's window log)", "bye alice" in alice.window_log())

    if server.poll() is None:
        server.send_signal(signal.SIGINT)
    try:
        code = server.wait(timeout=10 * SCALE)
    except subprocess.TimeoutExpired:
        server.kill()
        code = "killed"
    log_reader.join(timeout=5)
    check("server exits 0", code == 0, code)
    report = re.search(r"(AddressSanitizer|LeakSanitizer|runtime error).*", "".join(server_log))
    check("no sanitizer report", report is None, report.group(0) if report else "")
    shutil.rmtree(workdir, ignore_errors=True)

    failed = results.count(False)
    print("\n%d checks, %d failed" % (len(results), failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
