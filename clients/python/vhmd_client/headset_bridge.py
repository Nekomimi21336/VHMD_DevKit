"""Relay between the VHMD Link headset app and the VHMD driver.

Network (default): the app finds this PC by broadcasting "VHMD-DISCOVER" to
UDP port 39582 and gets the ports in the reply; it then sends one JSON array
of VHMD protocol messages per UDP datagram to port 39580. USB: over
`adb reverse` (done here unless --no-adb) the app sends the same arrays one
per line over TCP to port 39580. Either way each array goes on to the driver
(which only listens on this PC) as one UDP datagram. On TCP port 39581 the app
gets VHMD's image as H.264 (headset_stream; needs ffmpeg and the driver in
direct mode).

    python -m vhmd_client.headset_bridge [--usb-only] [--no-adb] [--no-video]

The network ports accept any device on the LAN: use it on a trusted network,
or --usb-only.
"""

from __future__ import annotations

import argparse
import json
import platform
import socket
import subprocess
import threading
import time

from . import headset_info
from .client import DEFAULT_HOST, DEFAULT_PORT
from .headset_stream import VIDEO_PORT, VideoServer

LISTEN_PORT = 39580
DISCOVERY_PORT = 39582


class Stats:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.lines = 0
        self.bytes = 0
        self.last = ""
        self.headset: dict = {}   # the app's own numbers (link_stats), every 2 s

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


def forward(data: bytes, udp: socket.socket, driver, stats: Stats) -> None:
    """One JSON array from the app to the driver; the app's "headset" message
    (its display: fields of view, eyes, render size, refresh rate) is kept here."""
    if b'"type":"headset"' in data or b'"type":"link_stats"' in data:
        try:
            msgs = json.loads(data)
        except ValueError:
            msgs = []
        rest = [m for m in msgs if m.get("type") not in ("headset", "link_stats")]
        for m in msgs:
            if m.get("type") == "headset":
                headset_info.update(m, log=lambda s: print(s, flush=True))
            elif m.get("type") == "link_stats":
                stats.headset = m
        data = json.dumps(rest, separators=(",", ":")).encode()
    udp.sendto(data, driver)
    stats.add(data)


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
                    forward(line, udp, driver, stats)


def serve_udp(sock: socket.socket, udp: socket.socket, driver, stats: Stats) -> None:
    """Poses over the network: one JSON array per datagram."""
    peer = None
    while True:
        data, addr = sock.recvfrom(65536)
        if addr != peer:
            peer = addr
            print("headset sending over the network from", addr[0], flush=True)
        forward(data, udp, driver, stats)


def serve_discovery(sock: socket.socket, args) -> None:
    """Answers the app's broadcast with this PC's ports (unicast)."""
    reply = json.dumps({"vhmd": "pc", "name": platform.node(), "pose_port": args.port,
                        "video_port": VIDEO_PORT}).encode()
    while True:
        data, addr = sock.recvfrom(1024)
        if data.startswith(b"VHMD-DISCOVER"):
            sock.sendto(reply, addr)


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
    ap.add_argument("--usb-only", action="store_true", help="listen on 127.0.0.1 only (adb reverse), no network")
    ap.add_argument("--no-video", action="store_true", help="poses only, no video")
    ap.add_argument("--bitrate", type=float, default=30.0, help="video bitrate, Mbit/s")
    ap.add_argument("--encoder", default="h264_amf", help="ffmpeg H.264 encoder (h264_amf, h264_nvenc, libx264, ...)")
    ap.add_argument("--buffer", type=int, default=0, choices=(0, 1, 2),
                    help="frames the headset holds back: 0 = newest (lowest latency), 1 = smoother (+1 frame)")
    args = ap.parse_args()

    if not args.no_adb:
        for port in (args.port, VIDEO_PORT):
            try:
                r = subprocess.run(["adb", "reverse", f"tcp:{port}", f"tcp:{port}"], capture_output=True,
                                   text=True, timeout=5)
                print(f"adb reverse {port}:", "ok" if r.returncode == 0 else (r.stderr.strip() or r.stdout.strip()))
            except (OSError, subprocess.TimeoutExpired):
                print(f"adb reverse {port}: adb not available (USB not used)")
    host = "127.0.0.1" if args.usb_only else "0.0.0.0"
    video = None
    if not args.no_video:
        video = VideoServer(host=host, bitrate_mbps=args.bitrate, encoder=args.encoder, buffer=args.buffer,
                            log=lambda m: print(m, flush=True))
        video.start()

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    driver = (args.driver_host, args.driver_port)
    stats = Stats()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, args.port))
    srv.listen(2)
    if not args.usb_only:
        pose_udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        pose_udp.bind((host, args.port))
        threading.Thread(target=serve_udp, args=(pose_udp, udp, driver, stats), daemon=True).start()
        disc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        disc.bind((host, DISCOVERY_PORT))
        threading.Thread(target=serve_discovery, args=(disc, args), daemon=True).start()
        print(f"network: discovery on UDP {DISCOVERY_PORT}, poses on UDP {args.port}, video on TCP {VIDEO_PORT}"
              " (any device on the LAN can connect)")
    print(f"USB: poses on TCP 127.0.0.1:{args.port} -> driver {driver[0]}:{driver[1]}", flush=True)

    def report() -> None:
        while True:
            time.sleep(1.0)
            n, b, last = stats.take()
            line = f"{n:4d} msg/s  {b / 1024:6.1f} KiB/s  {summary(last)}" if n else ""
            if video:
                fin, fout, vb = video.take_stats()
                if fin or fout:
                    line += f"  |  video in {fin} / out {fout} fps  {vb * 8 / 1e6:5.1f} Mbit/s"
            h = stats.headset
            if h:
                line += (f"  |  headset: received {h.get('received', 0):.0f} decoded {h.get('decoded', 0):.0f} "
                         f"shown {h.get('shown', 0):.0f}/s at {h.get('display', 0):.0f} Hz "
                         f"(repeats {h.get('repeats', 0):.0f}/s, buffer {h.get('buffer', 0)}, "
                         f"wait {h.get('wait_ms', 0):.1f} ms), {h.get('reprojected', 0):.0f}% reprojected")
            if line:
                print(line, flush=True)

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
