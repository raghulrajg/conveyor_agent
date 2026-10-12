#!/usr/bin/env python3
"""
TCP test SERVER for opta_tcp_client_test.ino (Opta = TCP client, like with the camera).

The PC plays the camera's role: it listens on port 8000, waits for the Opta to
connect, then runs network tests and prints PASS/FAIL.
Python 3.8+, standard library only.

  Setup
    PC Ethernet adapter: IP 169.254.163.5, mask 255.255.0.0 (the camera's IP)
    Allow Python through the PC firewall for TCP port 8000.
    Upload opta_tcp_client_test.ino (SNIFF_ONLY = false), connect the PC to the Opta.

  Tests (default: all except soak)
    hello      Opta connects and sends its HELLO line
    ping       PING -> PONG round-trip time (min / avg / p95 / max, lost replies)
    echo       payloads of 1..500 characters must come back unchanged
    parse      camera-style "x;y;theta;result;" strings (with and without CR/LF) are
               parsed correctly on the Opta
    burst      many lines sent back-to-back, all answers arrive in order
    led        STATUS LED 4 blinks twice (look at the Opta)
    info       prints the Opta's INFO line
    reconnect  the PC closes the connection; the Opta must reconnect by itself
    soak       one PING per second for --duration seconds (cable stability)

  Examples
    python tcp_test_server.py
    python tcp_test_server.py --tests ping,parse --count 100
    python tcp_test_server.py --tests soak --duration 600
    python tcp_test_server.py --console      # type lines to the Opta yourself
"""

import argparse
import socket
import string
import random
import sys
import threading
import time

SKIP_PREFIXES = ("HELLO",)


class Link:
    """One accepted connection from the Opta, with line reading."""

    def __init__(self, conn, peer):
        self.sock, self.peer = conn, peer
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""

    def send_line(self, text, terminator="\r\n"):
        self.sock.sendall(text.encode("ascii") + terminator.encode("ascii"))

    def read_line(self, timeout):
        deadline = time.perf_counter() + timeout
        while b"\n" not in self.buf:
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                return None
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(4096)
            except socket.timeout:
                return None
            if not chunk:
                raise ConnectionError("Opta closed the connection")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return line.strip(b"\r").decode("ascii", errors="replace")

    def read_reply(self, timeout):
        deadline = time.perf_counter() + timeout
        while True:
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                return None
            line = self.read_line(remaining)
            if line is None or not line.startswith(SKIP_PREFIXES):
                return line

    def close(self):
        try:
            self.sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


class Server:
    def __init__(self, args):
        self.args = args
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind((args.bind, args.port))
        self.srv.listen(2)
        self.link = None
        self.hello = None

    def wait_for_opta(self, timeout):
        self.srv.settimeout(timeout)
        t0 = time.perf_counter()
        conn, peer = self.srv.accept()
        waited = time.perf_counter() - t0
        if self.link:
            self.link.close()
        self.link = Link(conn, peer)
        self.hello = self.link.read_line(3.0)
        return waited


def stats(values):
    if not values:
        return "n/a"
    s = sorted(values)
    p95 = s[min(len(s) - 1, int(len(s) * 0.95))]
    return f"min {s[0]:.1f} / avg {sum(s) / len(s):.1f} / p95 {p95:.1f} / max {s[-1]:.1f} ms"


# ---------------------------------------------------------------------- tests
def test_hello(srv, args):
    ok = bool(srv.hello and srv.hello.startswith("HELLO"))
    return ok, f"from {srv.link.peer[0]}: {srv.hello or 'no HELLO (is SNIFF_ONLY = true?)'}"


def test_ping(srv, args):
    rtts, lost = [], 0
    for _ in range(args.count):
        t = time.perf_counter()
        srv.link.send_line("PING")
        if srv.link.read_reply(2.0) == "PONG":
            rtts.append((time.perf_counter() - t) * 1000)
        else:
            lost += 1
    return lost == 0, f"{len(rtts)}/{args.count} replies, {stats(rtts)}"


def test_echo(srv, args):
    alphabet = string.ascii_letters + string.digits + " -_.,;:!#$%&()[]{}=+*/"
    sizes = [1, 2, 10, 50, 100, 200, 255, 256, 300, 500]
    bad = 0
    for size in sizes:
        for _ in range(3):
            payload = "".join(random.choice(alphabet) for _ in range(size)).strip() or "X"
            srv.link.send_line("ECHO " + payload)
            reply = srv.link.read_reply(3.0)
            if reply != "ECHO:" + payload:
                bad += 1
                if bad <= 3:
                    print(f"       mismatch (size {size}): sent {payload[:40]!r} got {str(reply)[:50]!r}")
    total = len(sizes) * 3
    return bad == 0, f"{total - bad}/{total} payloads intact (1..500 chars)"


PARSE_CASES = [
    ("12.5;30.2;45.0;OK;", "\r\n", "XYZR x=12.500 y=30.200 theta=45.000 result=OK"),
    ("-1.25;0;359.999;NG;", "\r\n", "XYZR x=-1.250 y=0.000 theta=359.999 result=NG"),
    ("100.123;200.456;-90.5;OK;", "", "XYZR x=100.123 y=200.456 theta=-90.500 result=OK"),   # no CR/LF, like the camera
    ("1,2,3,1", "\n", "XYZR x=1.000 y=2.000 theta=3.000 result=1"),
    ("12.5;30.2;45.0;", "\r\n", "XYZR ERR"),                                               # result missing
    ("12.5;abc;45;OK;", "\r\n", "XYZR ERR"),
]


def test_parse(srv, args):
    bad = 0
    for text, term, expected in PARSE_CASES:
        srv.link.send_line("PARSE " + text, term)
        reply = srv.link.read_reply(2.0)
        if reply != expected:
            bad += 1
            print(f"       PARSE {text!r}: expected {expected!r}, got {reply!r}")
    return bad == 0, f"{len(PARSE_CASES) - bad}/{len(PARSE_CASES)} camera-format strings parsed correctly"


def test_burst(srv, args):
    n = args.burst
    lines = [f"BURST-{i:04d}-{'x' * (i % 40)}" for i in range(n)]
    t0 = time.perf_counter()
    srv.link.sock.sendall("".join("ECHO " + line + "\r\n" for line in lines).encode("ascii"))
    got = []
    while len(got) < n:
        reply = srv.link.read_reply(5.0)
        if reply is None:
            break
        got.append(reply)
    elapsed = (time.perf_counter() - t0) * 1000
    ok = got == ["ECHO:" + line for line in lines]
    return ok, f"{len(got)}/{n} answers{' in order' if ok else ' (MISSING or OUT OF ORDER)'} in {elapsed:.0f} ms"


def test_led(srv, args):
    ok = True
    for v in (1, 0, 1, 0):
        srv.link.send_line(f"LED 4 {v}")
        ok &= (srv.link.read_reply(2.0) == "OK")
        time.sleep(0.4)
    return ok, "STATUS LED 4 should have blinked twice"


def test_info(srv, args):
    srv.link.send_line("INFO")
    reply = srv.link.read_reply(2.0)
    return bool(reply and reply.startswith("INFO")), reply or "no reply"


def test_reconnect(srv, args):
    times, good = [], 0
    for _ in range(args.reconnects):
        srv.link.close()
        t0 = time.perf_counter()
        try:
            srv.wait_for_opta(10.0)
        except socket.timeout:
            print("       Opta did not reconnect within 10 s")
            break
        srv.link.send_line("PING")
        if srv.hello and srv.hello.startswith("HELLO") and srv.link.read_reply(2.0) == "PONG":
            good += 1
            times.append((time.perf_counter() - t0) * 1000)
    return good == args.reconnects, f"{good}/{args.reconnects} automatic reconnects, time {stats(times)}"


def test_soak(srv, args):
    rtts, lost, reconnects = [], 0, 0
    end = time.time() + args.duration
    print(f"       soak test for {args.duration} s (Ctrl+C to stop early)")
    try:
        while time.time() < end:
            t = time.perf_counter()
            try:
                srv.link.send_line("PING")
                if srv.link.read_reply(2.0) == "PONG":
                    rtts.append((time.perf_counter() - t) * 1000)
                else:
                    lost += 1
                    print(f"       {time.strftime('%H:%M:%S')} PING lost")
            except (OSError, ConnectionError) as exc:
                lost += 1
                print(f"       {time.strftime('%H:%M:%S')} {exc} - waiting for the Opta to reconnect")
                try:
                    srv.wait_for_opta(15.0)
                    reconnects += 1
                except socket.timeout:
                    print("       Opta did not reconnect within 15 s")
                    break
            time.sleep(max(0.0, 1.0 - (time.perf_counter() - t)))
    except KeyboardInterrupt:
        print("       stopped by user")
    return lost == 0, f"{len(rtts)} ok, {lost} lost, {reconnects} reconnects, {stats(rtts)}"


TESTS = {
    "hello": test_hello, "ping": test_ping, "echo": test_echo, "parse": test_parse,
    "burst": test_burst, "led": test_led, "info": test_info, "reconnect": test_reconnect,
    "soak": test_soak,
}
DEFAULT_TESTS = "hello,ping,echo,parse,burst,led,info,reconnect"


def console(srv):
    print("Console: lines you type are sent to the Opta (PING, ECHO hi, PARSE 1;2;3;OK;, INFO, LED 4 1 ...).")
    print("Everything the Opta sends is printed. 'quit' to exit.")

    def reader():
        while True:
            try:
                line = srv.link.read_line(3600)
            except (OSError, ConnectionError) as exc:
                print(f"\n[{time.strftime('%H:%M:%S')}] {exc}")
                return
            if line is not None:
                print(f"[{time.strftime('%H:%M:%S')}] OPTA: {line}")

    threading.Thread(target=reader, daemon=True).start()
    try:
        while True:
            text = input()
            if text.strip().lower() in ("quit", "exit", "q"):
                break
            srv.link.send_line(text)
    except (EOFError, KeyboardInterrupt, OSError):
        pass


def main():
    p = argparse.ArgumentParser(description="TCP test server for the Opta TCP client test",
                                formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    p.add_argument("--bind", default="0.0.0.0", help="local address (default all)")
    p.add_argument("--port", type=int, default=8000, help="TCP port (default 8000)")
    p.add_argument("--tests", default=DEFAULT_TESTS, help=f"comma list, default: {DEFAULT_TESTS}")
    p.add_argument("--wait", type=float, default=60, help="seconds to wait for the Opta (default 60)")
    p.add_argument("--count", type=int, default=20, help="pings in the ping test")
    p.add_argument("--burst", type=int, default=50, help="lines in the burst test")
    p.add_argument("--reconnects", type=int, default=5, help="cycles in the reconnect test")
    p.add_argument("--duration", type=int, default=60, help="soak test duration in seconds")
    p.add_argument("--console", action="store_true", help="interactive console instead of tests")
    args = p.parse_args()

    names = [t.strip().lower() for t in args.tests.split(",") if t.strip()]
    unknown = [n for n in names if n not in TESTS]
    if unknown:
        p.error(f"unknown test(s): {', '.join(unknown)}; choose from {', '.join(TESTS)}")

    try:
        srv = Server(args)
    except OSError as exc:
        print(f"Cannot listen on {args.bind}:{args.port}: {exc}")
        print("Is another program (or a second copy of this script) using the port?")
        sys.exit(1)

    print(f"== Waiting up to {args.wait:.0f} s for the Opta to connect to port {args.port} ...")
    print("   (PC must have the IP the Opta connects to, default 169.254.163.5 / 255.255.0.0)")
    try:
        waited = srv.wait_for_opta(args.wait)
    except socket.timeout:
        print("FAIL  the Opta did not connect. Check: PC IP/mask, cable, firewall (allow TCP port "
              f"{args.port}), Opta Serial Monitor messages, SERVER_IP in the sketch.")
        sys.exit(1)
    print(f"== Opta connected from {srv.link.peer[0]} after {waited:.1f} s")

    if args.console:
        console(srv)
        srv.link.close()
        return

    results = []
    for name in names:
        try:
            ok, detail = TESTS[name](srv, args)
        except (OSError, ConnectionError) as exc:
            ok, detail = False, f"{type(exc).__name__}: {exc}"
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'}  {name:<9} {detail}")
    srv.link.close()
    passed = sum(results)
    print(f"== {passed}/{len(results)} tests passed -> {'ALL OK' if passed == len(results) else 'CHECK FAILURES'}")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
