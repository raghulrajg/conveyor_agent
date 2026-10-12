#!/usr/bin/env python3
"""
TCP test CLIENT for opta_tcp_server_test.ino (Opta = server, PC = client).

  Setup: PC Ethernet adapter 169.254.163.50 / 255.255.0.0, Opta at 169.254.163.10
  Python 3.8+, standard library only.

  Tests (default: all except soak)
    connect    TCP connect time + clear diagnosis when it fails
    ping       PING -> PONG round-trip time (min / avg / p95 / max, lost replies)
    echo       payloads of 1..500 characters must come back unchanged
    parse      camera-style "x;y;theta;result;" strings are parsed correctly on the Opta
    burst      many lines sent back-to-back, all echoes arrive in order
    led        switches STATUS LED 4 on/off (look at the Opta)
    reconnect  repeated connect / disconnect cycles
    info       prints the Opta's INFO line
    soak       one PING per second for --duration seconds (cable stability)

  Examples
    python tcp_test_client.py
    python tcp_test_client.py --host 169.254.163.10 --tests ping,echo --count 100
    python tcp_test_client.py --tests soak --duration 600
    python tcp_test_client.py --listen          # only print what the Opta sends
"""

import argparse
import random
import socket
import string
import sys
import time

DEFAULT_HOST = "169.254.163.10"
DEFAULT_PORT = 8000
SKIP_PREFIXES = ("HELLO", "FROM ")


class Conn:
    def __init__(self, host, port, timeout=3.0):
        t0 = time.perf_counter()
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.connect_ms = (time.perf_counter() - t0) * 1000
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
                raise ConnectionError("connection closed by the Opta")
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
            self.sock.close()
        except OSError:
            pass


def diagnose(host, port, exc):
    if isinstance(exc, ConnectionRefusedError):
        return f"REFUSED: {host} answered but no server on port {port}. Is opta_tcp_server_test.ino uploaded?"
    if isinstance(exc, (socket.timeout, TimeoutError)):
        return (f"TIMEOUT: no answer from {host}. Check cable, Opta STATUS1 LED (link), "
                "PC IP in the same subnet (169.254.163.50 / 255.255.0.0), firewall/VPN.")
    if isinstance(exc, OSError) and getattr(exc, "errno", None) in (101, 113, 10051, 10065):
        return f"UNREACHABLE: the PC has no route to {host} ({exc}). Set a static IP on the PC's Ethernet adapter."
    return f"{type(exc).__name__}: {exc}"


def stats(values):
    if not values:
        return "n/a"
    s = sorted(values)
    p95 = s[min(len(s) - 1, int(len(s) * 0.95))]
    return f"min {s[0]:.1f} / avg {sum(s) / len(s):.1f} / p95 {p95:.1f} / max {s[-1]:.1f} ms"


def test_connect(a):
    c = Conn(a.host, a.port)
    banner = c.read_line(2.0)
    c.close()
    return True, f"{c.connect_ms:.1f} ms, banner: {banner or '(none)'}"


def test_ping(a):
    c = Conn(a.host, a.port)
    rtts, lost = [], 0
    for _ in range(a.count):
        t = time.perf_counter()
        c.send_line("PING")
        if c.read_reply(2.0) == "PONG":
            rtts.append((time.perf_counter() - t) * 1000)
        else:
            lost += 1
    c.close()
    return lost == 0, f"{len(rtts)}/{a.count} replies, {stats(rtts)}"


def test_echo(a):
    c = Conn(a.host, a.port)
    sizes = [1, 2, 10, 50, 100, 200, 255, 256, 300, 500]
    alphabet = string.ascii_letters + string.digits + " -_.,;:!#$%&()[]{}=+*/"
    bad = 0
    for size in sizes:
        for _ in range(3):
            payload = "X" + "".join(random.choice(alphabet) for _ in range(size - 1)).rstrip()
            c.send_line(payload)
            if c.read_reply(3.0) != "ECHO:" + payload:
                bad += 1
    c.close()
    return bad == 0, f"{len(sizes) * 3 - bad}/{len(sizes) * 3} payloads intact (1..500 chars)"


def test_parse(a):
    cases = [("12.5;30.2;45.0;OK;", "\r\n", "XYZR x=12.500 y=30.200 theta=45.000 result=OK"),
             ("-1.25;0;359.999;NG;", "\r\n", "XYZR x=-1.250 y=0.000 theta=359.999 result=NG"),
             ("100.123;200.456;-90.5;ok", "", "XYZR x=100.123 y=200.456 theta=-90.500 result=OK"),
             ("1,2,3,1", "\n", "XYZR x=1.000 y=2.000 theta=3.000 result=1"),
             ("12.5;30.2;45.0;", "\r\n", "XYZR ERR"),
             ("NG", "\r\n", "XYZR ERR")]
    c = Conn(a.host, a.port)
    bad = 0
    for text, term, expected in cases:
        c.send_line("PARSE " + text, term)
        reply = c.read_reply(2.0)
        if reply != expected:
            bad += 1
            print(f"       PARSE {text!r}: expected {expected!r}, got {reply!r}")
    c.close()
    return bad == 0, f"{len(cases) - bad}/{len(cases)} camera-format strings parsed correctly"


def test_burst(a):
    c = Conn(a.host, a.port)
    lines = [f"BURST-{i:04d}-{'x' * (i % 40)}" for i in range(a.burst)]
    t0 = time.perf_counter()
    c.sock.sendall("".join(l + "\r\n" for l in lines).encode("ascii"))
    got = []
    while len(got) < a.burst:
        r = c.read_reply(5.0)
        if r is None:
            break
        got.append(r)
    c.close()
    ok = got == ["ECHO:" + l for l in lines]
    return ok, f"{len(got)}/{a.burst} echoes{' in order' if ok else ' (MISSING/OUT OF ORDER)'} in {(time.perf_counter() - t0) * 1000:.0f} ms"


def test_led(a):
    c = Conn(a.host, a.port)
    ok = True
    for v in (1, 0, 1, 0):
        c.send_line(f"LED 4 {v}")
        ok &= (c.read_reply(2.0) == "OK")
        time.sleep(0.4)
    c.close()
    return ok, "STATUS LED 4 should have blinked twice"


def test_reconnect(a):
    good, times = 0, []
    for _ in range(a.reconnects):
        try:
            c = Conn(a.host, a.port)
            banner = c.read_line(2.0)
            c.send_line("PING")
            if banner and banner.startswith("HELLO") and c.read_reply(2.0) == "PONG":
                good += 1
                times.append(c.connect_ms)
            c.close()
        except OSError:
            pass
        time.sleep(0.2)
    return good == a.reconnects, f"{good}/{a.reconnects} cycles OK, connect {stats(times)}"


def test_info(a):
    c = Conn(a.host, a.port)
    c.send_line("INFO")
    r = c.read_reply(2.0)
    c.close()
    return bool(r and r.startswith("INFO")), r or "no reply"


def test_soak(a):
    c = Conn(a.host, a.port)
    rtts, lost = [], 0
    end = time.time() + a.duration
    print(f"       soak test for {a.duration} s (Ctrl+C to stop early)")
    try:
        while time.time() < end:
            t = time.perf_counter()
            try:
                c.send_line("PING")
                if c.read_reply(2.0) == "PONG":
                    rtts.append((time.perf_counter() - t) * 1000)
                else:
                    lost += 1
                    print(f"       {time.strftime('%H:%M:%S')} PING lost")
            except (OSError, ConnectionError) as exc:
                lost += 1
                print(f"       {time.strftime('%H:%M:%S')} {exc} - reconnecting")
                c.close()
                time.sleep(1.0)
                try:
                    c = Conn(a.host, a.port)
                except OSError:
                    pass
            time.sleep(max(0.0, 1.0 - (time.perf_counter() - t)))
    except KeyboardInterrupt:
        print("       stopped by user")
    c.close()
    return lost == 0, f"{len(rtts)} ok, {lost} lost, {stats(rtts)}"


TESTS = {"connect": test_connect, "ping": test_ping, "echo": test_echo, "parse": test_parse,
         "burst": test_burst, "led": test_led, "reconnect": test_reconnect, "info": test_info,
         "soak": test_soak}
DEFAULT_TESTS = "connect,ping,echo,parse,burst,led,reconnect,info"


def listen(a):
    print(f"Listening to {a.host}:{a.port} - Ctrl+C to stop")
    while True:
        try:
            c = Conn(a.host, a.port)
            print(f"{time.strftime('%H:%M:%S')} connected")
            while True:
                line = c.read_line(3600)
                if line is not None:
                    print(f"{time.strftime('%H:%M:%S')} {line}")
        except KeyboardInterrupt:
            return 0
        except (OSError, ConnectionError) as exc:
            print(f"{time.strftime('%H:%M:%S')} {diagnose(a.host, a.port, exc)} - retry in 2 s")
            time.sleep(2.0)


def main():
    p = argparse.ArgumentParser(description="TCP test client for the Opta TCP server test",
                                formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    p.add_argument("--host", default=DEFAULT_HOST)
    p.add_argument("--port", type=int, default=DEFAULT_PORT)
    p.add_argument("--tests", default=DEFAULT_TESTS)
    p.add_argument("--count", type=int, default=20)
    p.add_argument("--burst", type=int, default=50)
    p.add_argument("--reconnects", type=int, default=10)
    p.add_argument("--duration", type=int, default=60)
    p.add_argument("--listen", action="store_true")
    a = p.parse_args()
    if a.listen:
        sys.exit(listen(a))

    names = [t.strip().lower() for t in a.tests.split(",") if t.strip()]
    bad = [n for n in names if n not in TESTS]
    if bad:
        p.error(f"unknown test(s): {', '.join(bad)}; choose from {', '.join(TESTS)}")

    print(f"== Opta TCP server test  {a.host}:{a.port}")
    results = []
    for name in names:
        try:
            ok, detail = TESTS[name](a)
        except (OSError, ConnectionError) as exc:
            ok, detail = False, diagnose(a.host, a.port, exc)
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'}  {name:<9} {detail}")
        if name == "connect" and not ok:
            break
    passed = sum(results)
    print(f"== {passed}/{len(results)} tests passed -> {'ALL OK' if passed == len(results) else 'CHECK FAILURES'}")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
