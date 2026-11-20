#!/usr/bin/env python3
"""Random clients against ircserv for a while; then checks it survived intact.

Usage:  python3 tests/fuzz/chaos.py ./ircserv [seconds] [seed]

Up to 80 simulated clients at a time randomly connect, register (sometimes
with a wrong password), send random IRC-shaped commands (often split into
tiny pieces), flood, stop reading for a while, and disconnect abruptly
(normal close or reset). Afterwards the server must:
  - answer a fresh client normally
  - hold no leftover connections (open fd count back to its starting value)
  - exit 0 on SIGINT
  - print no sanitizer report (when built with make asan)
"""
import os
import random
import re
import selectors
import signal
import socket
import struct
import subprocess
import sys
import threading
import time

NICKS = ["n%d" % i for i in range(12)] + ["N1", "[x]", "{X}", "1bad", "*", "a b"]
CHANNELS = ["#a", "#A", "#b", "&c", "#", "x", "#a,#b", "#bad:x", "0"]
COMMANDS = ["PASS", "NICK", "USER", "CAP", "PING", "PONG", "QUIT", "JOIN", "PART", "TOPIC", "NAMES",
            "WHO", "PRIVMSG", "NOTICE", "KICK", "INVITE", "MODE", "FOO", "privmsg", ""]


def random_text(rng):
    size = rng.choice([0, 1, 5, 20, 80, 300, 600])
    return bytes(rng.choice([b for b in range(256) if b not in (10, 13)]) for _ in range(size))


def random_line(rng):
    nick, channel = rng.choice(NICKS), rng.choice(CHANNELS)
    choice = rng.randrange(14)
    if choice == 0:
        return b"NICK " + nick.encode()
    if choice == 1:
        return ("JOIN %s%s" % (channel, rng.choice(["", " key", " k1,k2"]))).encode()
    if choice == 2:
        return ("PART %s :bye" % channel).encode()
    if choice in (3, 4):
        return ("%s %s :" % (rng.choice(["PRIVMSG", "NOTICE"]), rng.choice([nick, channel]))).encode() + random_text(rng)
    if choice == 5:
        return ("TOPIC %s :" % channel).encode() + random_text(rng)
    if choice == 6:
        return ("KICK %s %s :out" % (channel, nick)).encode()
    if choice == 7:
        return ("INVITE %s %s" % (nick, channel)).encode()
    if choice == 8:
        modes = "".join(rng.choice("+-itkolbx") for _ in range(rng.randrange(1, 7)))
        params = " ".join(rng.choice([nick, "key", "2", "0", "x,y"]) for _ in range(rng.randrange(4)))
        return ("MODE %s %s %s" % (channel, modes, params)).encode()
    if choice == 9:
        return ("WHO %s" % rng.choice([nick, channel])).encode()
    if choice == 10:
        return b"PING :" + random_text(rng)[:20]
    if choice == 11:
        return b"A" * rng.choice([400, 600, 3000])
    if choice == 12:
        return (":spoof!x@y PRIVMSG %s :hi" % channel).encode()
    tokens = " ".join(rng.choice(NICKS + CHANNELS) for _ in range(rng.randrange(5)))
    return (rng.choice(COMMANDS) + " " + tokens).encode()


class Agent:
    def __init__(self, port, rng, registry):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.sock.setblocking(False)
        self.rng = rng
        self.paused_until = 0
        self.alive = True
        registry.register(self.sock, selectors.EVENT_READ, self)
        if rng.random() < 0.7:
            password = "pw" if rng.random() < 0.9 else "wrong"
            nick = rng.choice(NICKS[:12])
            self.send(("PASS %s\r\nNICK %s\r\nUSER %s 0 * :chaos\r\n" % (password, nick, nick)).encode())

    def send(self, data):
        try:
            self.sock.send(data)
        except (BlockingIOError, ConnectionError, OSError):
            pass

    def act(self, now):
        roll = self.rng.random()
        if roll < 0.70:
            data = b"".join(random_line(self.rng) + self.rng.choice([b"\r\n", b"\n", b"\r", b""])
                            for _ in range(self.rng.randrange(1, 4)))
            if self.rng.random() < 0.3:
                for i in range(0, len(data), 7):
                    self.send(data[i:i + 7])
            else:
                self.send(data)
        elif roll < 0.80:
            self.send(b"PRIVMSG #a :flood\r\n" * self.rng.choice([10, 50, 200]))
        elif roll < 0.90:
            self.paused_until = now + self.rng.uniform(0.5, 6)	# stop reading for a while
        else:
            return False	# disconnect
        return True

    def drain(self):
        try:
            while self.sock.recv(65536):
                pass
            self.alive = False	# EOF: the server closed us
        except BlockingIOError:
            pass
        except (ConnectionError, OSError):
            self.alive = False

    def close(self, registry):
        registry.unregister(self.sock)
        if self.rng.random() < 0.4:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        self.sock.close()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    server = sys.argv[1]
    duration = float(sys.argv[2]) if len(sys.argv) > 2 else 30
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rng = random.Random(seed)

    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()
    process = subprocess.Popen([server, str(port), "pw"], stderr=subprocess.PIPE, text=True)
    log = []
    reader = threading.Thread(target=lambda: [log.append(l) for l in process.stderr])
    reader.daemon = True
    reader.start()
    for _ in range(100):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.1)
    time.sleep(0.5)
    baseline = len(os.listdir("/proc/%d/fd" % process.pid))

    registry = selectors.DefaultSelector()
    agents, connections, actions = [], 0, 0
    end = time.time() + duration
    while time.time() < end:
        now = time.time()
        if len(agents) < 80 and rng.random() < 0.3:
            try:
                agents.append(Agent(port, rng, registry))
                connections += 1
            except OSError:
                pass
        for agent in rng.sample(agents, min(len(agents), 5)):
            actions += 1
            if not agent.act(now):
                agent.close(registry)
                agents.remove(agent)
        for key, _ in registry.select(timeout=0.01):
            agent = key.data
            if agent.paused_until <= now:
                agent.drain()
        for agent in [a for a in agents if not a.alive]:
            agent.close(registry)
            agents.remove(agent)
        if process.poll() is not None:
            break

    for agent in agents:
        agent.close(registry)
    results = []

    def check(name, ok, detail=""):
        results.append(ok)
        print(("  PASS " if ok else "  FAIL ") + name + ("" if ok else "  -> " + repr(detail)[:300]))

    print("chaos: %d connections, %d actions in %.0f s (seed %d)" % (connections, actions, duration, seed))
    check("server still running", process.poll() is None)
    if process.poll() is None:
        fresh = socket.create_connection(("127.0.0.1", port), timeout=10)
        fresh.sendall(b"PASS pw\r\nNICK fresh\r\nUSER f 0 * :f\r\nPING :end\r\n")
        data, deadline = b"", time.time() + 10
        fresh.settimeout(1)
        while b"PONG" not in data and time.time() < deadline:
            try:
                data += fresh.recv(65536)
            except socket.timeout:
                pass
        check("a fresh client registers and is answered", b" 001 fresh " in data and b"PONG" in data, data[-200:])
        fresh.close()
        deadline = time.time() + 15
        while len(os.listdir("/proc/%d/fd" % process.pid)) != baseline and time.time() < deadline:
            time.sleep(0.2)
        after = len(os.listdir("/proc/%d/fd" % process.pid))
        check("no leftover connections (fds back to baseline)", after == baseline, (baseline, after))
        process.send_signal(signal.SIGINT)
    try:
        code = process.wait(timeout=60)
    except subprocess.TimeoutExpired:
        process.kill()
        code = "killed"
    reader.join(timeout=5)
    check("exit code 0 after SIGINT", code == 0, code)
    report = re.search(r"(AddressSanitizer|LeakSanitizer|runtime error).*", "".join(log))
    check("no sanitizer report", report is None, report.group(0) if report else "")
    reasons = {}
    for line in log:
        match = re.search(r"disconnected: (.*)", line)
        if match:
            reason = re.sub(r"Quit: .*", "Quit: <message>", match.group(1))
            reasons[reason] = reasons.get(reason, 0) + 1
    print("  disconnect reasons: " + ", ".join("%s x%d" % kv for kv in sorted(reasons.items())))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
