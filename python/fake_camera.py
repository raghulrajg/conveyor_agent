#!/usr/bin/env python3
"""
Fake Hikrobot camera for testing the Opta deploy programs without the camera.
Sends results exactly like the SC3000:   x;y;theta;result;   e.g. 12.500;30.200;45.000;OK;
Python 3.8+, standard library only.

  Two modes - pick the one that matches the deploy sketch:

  --mode server   PC = camera TCP SERVER  ->  opta_deploy_tcp_client_mqtt.ino
                  PC Ethernet: 169.254.163.5 / 255.255.0.0 (the camera's IP)
                  The Opta connects to the PC on port 8000.

  --mode client   PC = camera TCP CLIENT  ->  opta_deploy_tcp_server_mqtt.ino
                  PC Ethernet: 169.254.163.5 (or .50) / 255.255.0.0
                  The PC connects to the Opta at 169.254.163.10:8000.

  Examples
    python fake_camera.py --mode server                    # press Enter to send a result
    python fake_camera.py --mode client --host 169.254.163.10
    python fake_camera.py --mode server --interval 1       # continuous camera, 1 result/s
    python fake_camera.py --mode server --auto --delay 0.5 # answer every trigger from the Opta
    python fake_camera.py --mode server --ng-rate 0.3      # 30 % of random results are NG

  Console commands
    <Enter> or s                send one random OK result
    ng                          send one random NG result
    xyz <x> <y> <theta> [res]   send these values, e.g.  xyz 12.5 30.2 45 OK
    raw <text>                  send text exactly as typed, e.g.  raw garbage
    auto on|off                 answer every trigger string received from the Opta
    interval <s>|off            send a result every <s> seconds (continuous mode)
    kick                        close the connection (tests reconnect on both sides)
    status                      connection info
    quit                        exit
"""

import argparse
import random
import socket
import sys
import threading
import time

_print_lock = threading.Lock()


def log(tag, text):
    with _print_lock:
        sys.stdout.write(f"{time.strftime('%H:%M:%S')} [{tag}] {text}\n")
        sys.stdout.flush()


class FakeCamera:
    def __init__(self, args):
        self.args = args
        self.term = {"none": "", "crlf": "\r\n", "lf": "\n", "cr": "\r"}[args.terminator]
        self.conn = None
        self.peer = None
        self.lock = threading.Lock()
        self.auto = args.auto
        self.interval = args.interval if args.interval and args.interval > 0 else None
        self.sent = 0
        self.running = True

    # ------------------------------------------------------------ results
    def make_result(self, x=None, y=None, theta=None, result=None):
        d, sep = self.args.decimals, self.args.sep
        x = random.uniform(0, 500) if x is None else x
        y = random.uniform(0, 500) if y is None else y
        theta = random.uniform(-180, 180) if theta is None else theta
        if result is None:
            result = "NG" if random.random() < self.args.ng_rate else "OK"
        text = f"{x:.{d}f}{sep}{y:.{d}f}{sep}{theta:.{d}f}{sep}{result}"
        return text + (sep if not self.args.no_trailing else "")

    def send(self, text, why="manual"):
        with self.lock:
            if not self.conn:
                log("CAM", "not connected to the Opta - nothing sent")
                return False
            try:
                self.conn.sendall((text + self.term).encode("ascii"))
                self.sent += 1
                log("TX ", f"{text!r} ({why})")
                return True
            except OSError as exc:
                log("CAM", f"send failed: {exc}")
                self._drop_locked()
                return False

    def _drop_locked(self):
        if self.conn:
            try:
                self.conn.shutdown(socket.SHUT_RDWR)       # FIN even while the reader waits
            except OSError:
                pass
            try:
                self.conn.close()
            except OSError:
                pass
        self.conn = None
        self.peer = None

    def _attach(self, conn, peer):
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with self.lock:
            if self.conn:
                log("CAM", f"new connection replaces {self.peer[0]}")
                self._drop_locked()
            self.conn, self.peer = conn, peer
        threading.Thread(target=self.reader, args=(conn, peer), daemon=True).start()

    # ------------------------------------------------------------ threads
    def server_loop(self, server):
        while self.running:
            try:
                conn, peer = server.accept()
            except OSError:
                return
            log("CAM", f"Opta connected from {peer[0]}:{peer[1]}")
            self._attach(conn, peer)

    def client_loop(self):
        warned = False
        while self.running:
            if self.conn:
                time.sleep(0.2)
                continue
            try:
                conn = socket.create_connection((self.args.host, self.args.port), timeout=3.0)
                conn.settimeout(None)
                log("CAM", f"connected to the Opta at {self.args.host}:{self.args.port}")
                warned = False
                self._attach(conn, (self.args.host, self.args.port))
            except OSError as exc:
                if not warned:
                    log("CAM", f"cannot connect to {self.args.host}:{self.args.port} ({exc}) - retrying every 2 s")
                    warned = True
                time.sleep(2.0)

    def reader(self, conn, peer):
        while self.running:
            try:
                data = conn.recv(1024)
            except OSError:
                data = b""
            if not data:
                with self.lock:
                    if self.conn is conn:
                        self._drop_locked()
                        log("CAM", "connection closed")
                return
            log("RX ", f"from Opta: {data!r}  (trigger?)")
            if self.auto:
                threading.Timer(self.args.delay, self.send, args=(self.make_result(), "auto reply")).start()

    def interval_loop(self):
        while self.running:
            if not self.interval:
                time.sleep(0.2)
                continue
            time.sleep(self.interval)
            if self.interval and self.conn:
                self.send(self.make_result(), f"interval {self.interval}s")

    # ------------------------------------------------------------ console
    def run(self):
        server = None
        if self.args.mode == "server":
            server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                server.bind((self.args.bind, self.args.port))
            except OSError as exc:
                print(f"Cannot listen on {self.args.bind}:{self.args.port}: {exc}")
                return 1
            server.listen(2)
            print(f"Fake camera = TCP SERVER on port {self.args.port}  (for opta_deploy_tcp_client_mqtt)")
            print("PC must have the camera's IP 169.254.163.5 / 255.255.0.0")
            threading.Thread(target=self.server_loop, args=(server,), daemon=True).start()
        else:
            print(f"Fake camera = TCP CLIENT -> {self.args.host}:{self.args.port}  (for opta_deploy_tcp_server_mqtt)")
            threading.Thread(target=self.client_loop, daemon=True).start()
        print(f"Result format: {self.make_result(12.5, 30.2, 45.0, 'OK')!r}  - type 'help' for commands\n")
        threading.Thread(target=self.interval_loop, daemon=True).start()

        try:
            while True:
                try:
                    line = input().strip()
                except EOFError:
                    break
                cmd, _, arg = line.partition(" ")
                cmd = cmd.lower()
                if cmd in ("", "s", "send"):
                    self.send(self.make_result(result="OK"))
                elif cmd == "ng":
                    self.send(self.make_result(result="NG"))
                elif cmd == "xyz":
                    parts = arg.replace(",", " ").split()
                    try:
                        x, y, t = (float(v) for v in parts[:3])
                        res = parts[3] if len(parts) > 3 else "OK"
                        self.send(self.make_result(x, y, t, res))
                    except ValueError:
                        log("ERR", "usage: xyz <x> <y> <theta> [OK|NG]")
                elif cmd == "raw":
                    self.send(arg, "raw")
                elif cmd == "auto":
                    self.auto = arg.lower() in ("on", "1", "yes", "true")
                    log("CFG", f"auto reply: {'ON' if self.auto else 'OFF'}")
                elif cmd == "interval":
                    try:
                        self.interval = None if arg.lower() in ("", "off", "0") else float(arg)
                    except ValueError:
                        log("ERR", "usage: interval <seconds>|off")
                    log("CFG", f"interval: {self.interval or 'OFF'}")
                elif cmd == "kick":
                    with self.lock:
                        self._drop_locked()
                    log("CAM", "connection closed by command")
                elif cmd == "status":
                    log("CFG", f"mode={self.args.mode} connected={self.peer[0] if self.peer else 'no'} "
                               f"sent={self.sent} auto={self.auto} interval={self.interval or 'off'}")
                elif cmd in ("help", "?", "h"):
                    print(__doc__.split("Console commands")[1])
                elif cmd in ("quit", "exit", "q"):
                    break
                else:
                    log("ERR", f"unknown command {cmd!r} - type help")
        except KeyboardInterrupt:
            pass
        finally:
            self.running = False
            with self.lock:
                self._drop_locked()
            if server:
                server.close()
            print("bye")
        return 0


def main():
    p = argparse.ArgumentParser(description="Fake Hikrobot camera for the Opta deploy programs",
                                formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    p.add_argument("--mode", choices=["server", "client"], required=True,
                   help="server: PC is the camera TCP server; client: PC connects to the Opta")
    p.add_argument("--host", default="169.254.163.10", help="Opta IP (client mode)")
    p.add_argument("--bind", default="0.0.0.0", help="listen address (server mode)")
    p.add_argument("--port", type=int, default=8000)
    p.add_argument("--sep", default=";", choices=[";", ","], help="field separator (default ;)")
    p.add_argument("--no-trailing", action="store_true", help="no separator after the result field")
    p.add_argument("--terminator", choices=["none", "crlf", "lf", "cr"], default="none",
                   help="line ending after each message (default none)")
    p.add_argument("--decimals", type=int, default=3)
    p.add_argument("--ng-rate", type=float, default=0.0, help="share of random results that are NG (0..1)")
    p.add_argument("--auto", action="store_true", help="answer every trigger received from the Opta")
    p.add_argument("--delay", type=float, default=0.5, help="auto-reply delay in seconds")
    p.add_argument("--interval", type=float, default=0, help="send a result every N seconds")
    sys.exit(FakeCamera(p.parse_args()).run())


if __name__ == "__main__":
    main()
