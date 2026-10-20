"""Relay between the VHMD Link headset app and the VHMD driver.

The app sends one JSON array of VHMD protocol messages per line over TCP to
port 39580 on the headset's localhost; `adb reverse tcp:39580 tcp:39580` (done
here unless --no-adb) makes that this PC. Each line goes on to the driver as
one UDP datagram.

    python -m vhmd_client.headset_bridge [--driver-port 39570] [--no-adb]
"""

from __future__ import annotations

import argparse
import json
import socket
import subprocess
import threading
import time

from .client import DEFAULT_HOST, DEFAULT_PORT

LISTEN_PORT = 39580


class Stats:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.lines = 0
        self.bytes = 0
        self.last = ""

    def add(self, line: bytes) -> None:
        with self.lock:
            self.lines += 1
            self.bytes += len(line)
            self.last = line.decode("utf-8", "replace")

    def take(self):
        with self.lock:
            out = (self.lines, self.bytes, self.last)
            self.lines = self.bytes = 0
            return out


def serve_client(conn: socket.socket, udp: socket.socket, driver, stats: Stats) -> None:
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    buf = b""
    with conn:
        while True:
            data = conn.recv(65536)
            if not data:
                break
            buf += data
            *lines, buf = buf.split(b"\n")
            for line in lines:
                if line:
                    udp.sendto(line, driver)
                    stats.add(line)


def summary(last: str) -> str:
    """Short human-readable view of the newest message (head position, trigger)."""
    try:
        msgs = json.loads(last)
    except ValueError:
        return ""
    parts = []
    for m in msgs:
        if m.get("type") == "pose" and m.get("id") == "hmd":
            parts.append("head " + (" ".join(f"{v:+.2f}" for v in m["pos"]) if "pos" in m else "invalid"))
        if m.get("type") == "input":
            parts.append(f"{m['id'][0].upper()} trig {m['values'].get('trigger', 0):.2f}")
    return "  ".join(parts)


def main() -> None:
    ap = argparse.ArgumentParser(description="Relay the VHMD Link headset app to the VHMD driver")
    ap.add_argument("--driver-host", default=DEFAULT_HOST)
    ap.add_argument("--driver-port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--port", type=int, default=LISTEN_PORT, help="TCP port the app connects to")
    ap.add_argument("--no-adb", action="store_true", help="do not run adb reverse")
    args = ap.parse_args()

    if not args.no_adb:
        r = subprocess.run(["adb", "reverse", f"tcp:{args.port}", f"tcp:{args.port}"], capture_output=True, text=True)
        print("adb reverse:", "ok" if r.returncode == 0 else (r.stderr.strip() or r.stdout.strip()))

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    driver = (args.driver_host, args.driver_port)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(2)
    stats = Stats()
    print(f"waiting for the headset on 127.0.0.1:{args.port} -> driver {driver[0]}:{driver[1]}")

    def report() -> None:
        while True:
            time.sleep(1.0)
            n, b, last = stats.take()
            if n:
                print(f"{n:4d} msg/s  {b / 1024:6.1f} KiB/s  {summary(last)}", flush=True)

    threading.Thread(target=report, daemon=True).start()
    try:
        while True:
            conn, addr = srv.accept()
            print("headset connected", addr, flush=True)
            threading.Thread(target=serve_client, args=(conn, udp, driver, stats), daemon=True).start()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
