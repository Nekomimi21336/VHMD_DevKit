"""Relay between the VHMD Link headset app and the VHMD driver.

Network (default): the app finds this PC by broadcasting "VHMD-DISCOVER" to
UDP port 39582 and gets the ports in the reply; it then sends one JSON array
of VHMD protocol messages per UDP datagram to port 39580. USB: over
`adb reverse` (done here unless --no-adb) the app sends the same arrays one
per line over TCP to port 39580. Either way each array goes on to the driver
(which only listens on this PC) as one UDP datagram. On TCP port 39581 the app
gets VHMD's image and the PC's sound (headset_stream).

Back to the headset go the driver's haptic events and, about once a second, the
PC's status: {"type": "status", "pc": name, "steamvr": bool, "driver": bool,
"control": owner of the HMD}. The headset's edition ("dev") makes the
controllers show as axes in SteamVR.

    python -m vhmd_client.headset_bridge [--usb-only] [--no-adb] [--no-video]

The PC app (`python -m vhmd_client.link`) runs the same Bridge with a window.
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
from dataclasses import dataclass
from typing import Callable, Optional

from . import headset_info
from .client import DEFAULT_HOST, DEFAULT_PORT
from .headset_stream import VIDEO_PORT, VideoServer

LISTEN_PORT = 39580
DISCOVERY_PORT = 39582


@dataclass
class BridgeOptions:
    driver_host: str = DEFAULT_HOST
    driver_port: int = DEFAULT_PORT
    port: int = LISTEN_PORT
    adb: bool = True              # run adb reverse for USB
    usb_only: bool = False        # listen on 127.0.0.1 only
    video: bool = True
    bitrate: float = 40.0         # Mbit/s
    codec: str = "hevc"
    eye_width: int = 0
    video_source: str = "auto"
    encoder: str = "h264_amf"
    buffer: int = 0
    adaptive: bool = True         # lower the bitrate when the network cannot keep up
    sync: bool = True             # move VHMD's vsync to the headset's display timing
    sync_target_ms: float = 0.0   # how long frames should wait on the headset (0: the driver's default)
    refresh: float = 0.0          # display refresh rate asked of the headset (0: its own)


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


class HeadsetLink:
    """Where the headset is: its UDP address (Wi-Fi) or TCP connection (USB),
    for messages back to it (haptics, the PC's status)."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.udp_sock = None
        self.udp_addr = None
        self.tcp = None
        self.sent = 0
        self.last_seen = 0.0      # time.monotonic() of the newest message from the headset

    def via_udp(self, sock: socket.socket, addr) -> None:
        with self.lock:
            self.udp_sock, self.udp_addr, self.tcp = sock, addr, None

    def via_tcp(self, conn: socket.socket) -> None:
        with self.lock:
            self.tcp, self.udp_addr = conn, None

    def kind(self) -> Optional[str]:
        with self.lock:
            return "usb" if self.tcp is not None else "wifi" if self.udp_addr is not None else None

    def address(self) -> str:
        with self.lock:
            return self.udp_addr[0] if self.udp_addr else ("USB" if self.tcp is not None else "")

    def send(self, data: bytes) -> None:
        with self.lock:
            try:
                if self.tcp is not None:
                    self.tcp.sendall(data + b"\n")
                elif self.udp_addr is not None:
                    self.udp_sock.sendto(data, self.udp_addr)
                else:
                    return
                self.sent += 1
            except OSError:
                pass


class Bridge:
    """The relay, as an object: start() it, read status() now and then."""

    def __init__(self, options: BridgeOptions, log: Callable[[str], None] = print) -> None:
        self.o = options
        self.log = log
        self.stats = Stats()
        self.link = HeadsetLink()
        self.video: Optional[VideoServer] = None
        self.info: dict = {}            # the headset's latest "headset" message
        self.pc_state = {"steamvr": False, "driver": False, "control": ""}
        self.rates = {"msgs": 0, "kib": 0.0, "video_in": 0, "video_out": 0, "mbps": 0.0}
        self.summary = ""
        self.sync_state: dict = {}      # the driver's last display_sync reply
        self.sync_shift_total = 0.0     # ms moved since the start (drift followed)
        self.forward_us = (0.0, 0.0)    # pose message: received -> sent to the driver (avg, max), last second
        self._fwd_samples: list = []
        self._models_applied: Optional[str] = None   # render models set for this driver session
        self._rate_note = ""                          # last refresh-rate message logged
        self._driver = (options.driver_host, options.driver_port)
        self._udp: Optional[socket.socket] = None
        self._replies: dict = {}
        self._reply_cv = threading.Condition()
        self._seq = 0

    # -- setup ----------------------------------------------------------------
    def start(self) -> None:
        o = self.o
        if o.adb:
            self._adb_reverse()
        host = "127.0.0.1" if o.usb_only else "0.0.0.0"
        if o.video:
            self.video = VideoServer(host=host, bitrate_mbps=o.bitrate, encoder=o.encoder, buffer=o.buffer,
                                     codec=o.codec, eye_width=o.eye_width, source=o.video_source,
                                     adaptive=o.adaptive, log=self.log)
            self.video.start()

        self._udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._udp.bind(("127.0.0.1", 0))
        self._thread(self._serve_driver_replies)
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, o.port))
        srv.listen(2)
        self._thread(self._accept, srv)
        if not o.usb_only:
            pose_udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            pose_udp.bind((host, o.port))
            self._thread(self._serve_udp, pose_udp)
            disc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            disc.bind((host, DISCOVERY_PORT))
            self._thread(self._serve_discovery, disc)
            self.log(f"network: discovery on UDP {DISCOVERY_PORT}, poses on UDP {o.port}, video on TCP {VIDEO_PORT}"
                     " (any device on the LAN can connect)")
        self.log(f"USB: poses on TCP 127.0.0.1:{o.port} -> driver {self._driver[0]}:{self._driver[1]}")
        self._thread(self._monitor)
        if o.adb:
            self._thread(self._adb_watch)

    def _adb_reverse(self, quiet: bool = False) -> bool:
        """USB: lets the headset reach this PC's ports as its own 127.0.0.1."""
        ok = True
        for port in (self.o.port, VIDEO_PORT):
            try:
                r = subprocess.run(["adb", "reverse", f"tcp:{port}", f"tcp:{port}"], capture_output=True, text=True,
                                   timeout=5, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
                ok = ok and r.returncode == 0
                if not quiet:
                    self.log(f"adb reverse {port}: " + ("ok" if r.returncode == 0 else (r.stderr.strip() or r.stdout.strip())))
            except (OSError, subprocess.TimeoutExpired):
                ok = False
                if not quiet:
                    self.log(f"adb reverse {port}: adb not available (USB not used)")
        return ok

    def _adb_watch(self) -> None:
        """A headset plugged in later: set up adb reverse again while none is connected."""
        was_ok = None
        while True:
            time.sleep(10.0)
            if self.connected():
                continue
            ok = self._adb_reverse(quiet=True)
            if ok and was_ok is False:
                self.log("USB: headset found, adb reverse set up")
            was_ok = ok

    @staticmethod
    def _thread(fn, *args) -> None:
        threading.Thread(target=fn, args=args, daemon=True).start()

    def set_video(self, bitrate: Optional[float] = None, codec: Optional[str] = None,
                  buffer: Optional[int] = None) -> None:
        """New video settings; the headset reconnects to get them (about a second)."""
        if bitrate is not None:
            self.o.bitrate = bitrate
        if codec is not None:
            self.o.codec = codec
        if buffer is not None:
            self.o.buffer = buffer
        if self.video:
            if codec is None and buffer is None:   # bitrate only: no reconnect
                self.video.set_bitrate(self.o.bitrate)
                return
            self.video.bitrate, self.video.codec, self.video.buffer = self.o.bitrate, self.o.codec, self.o.buffer
            self.video.restart()

    def request(self, op: str, timeout: float = 0.3, **args) -> dict:
        """A config request to the driver, sent from the socket the poses use: the
        driver sends haptic events to whoever sent last, so asking from another
        socket could take them away from the headset."""
        with self._reply_cv:
            self._seq += 1
            seq = 900000 + self._seq
        self._udp.sendto(json.dumps({"type": "config", "op": op, "seq": seq, **args}).encode(), self._driver)
        with self._reply_cv:
            if not self._reply_cv.wait_for(lambda: seq in self._replies, timeout):
                raise TimeoutError(f"no reply to '{op}' from the driver")
            return self._replies.pop(seq)

    # -- status ---------------------------------------------------------------
    def connected(self) -> bool:
        return time.monotonic() - self.link.last_seen < 2.0

    def status(self) -> dict:
        return {
            "headset": self.connected(),
            "link": self.link.kind() if self.connected() else None,
            "address": self.link.address(),
            "model": self.info.get("model", ""),
            "edition": self.info.get("edition", ""),
            "version": self.info.get("version", ""),
            "refresh": self.info.get("refresh", 0),
            "render": self.info.get("render", []),
            "video_connected": bool(self.video and self.video.current),
            **self.pc_state,
            **self.rates,
            "headset_stats": dict(self.stats.headset),
            "bitrate_now": self.video.bitrate_now if self.video else 0.0,
            "relay_ms": self.video.relay_ms if self.video else (0.0, 0.0, 0.0),
            "forward_us": self.forward_us,
            "sync": dict(self.sync_state),
            "sync_total_ms": self.sync_shift_total,
            "summary": self.summary,
        }

    # -- headset -> driver ----------------------------------------------------
    def _forward(self, data: bytes) -> None:
        """One JSON array from the app to the driver; the app's "headset" message
        (its display: fields of view, eyes, render size, refresh rate, edition)
        and statistics stay here."""
        t0 = time.perf_counter()
        self.link.last_seen = time.monotonic()
        if b'"type":"headset"' in data or b'"type":"link_stats"' in data or b'"type":"link_timing"' in data:
            try:
                msgs = json.loads(data)
            except ValueError:
                msgs = []
            rest = [m for m in msgs if m.get("type") not in ("headset", "link_stats", "link_timing")]
            for m in msgs:
                if m.get("type") == "headset":
                    self.info = m
                    headset_info.update(m, log=self.log)
                elif m.get("type") == "link_stats":
                    self.stats.headset = m
                elif m.get("type") == "link_timing" and self.o.sync:
                    self._display_sync(m)
            data = json.dumps(rest, separators=(",", ":")).encode()
        self._udp.sendto(data, self._driver)
        self.stats.add(data)
        self._fwd_samples.append((time.perf_counter() - t0) * 1e6)

    def _display_sync(self, m: dict) -> None:
        """The headset's frame timing to the driver, which moves its vsync clock
        (no waiting for the reply here: this is the pose path)."""
        req = {"type": "config", "op": "display_sync", "seq": -1, "slack_ms": m.get("slack_ms"),
               "period_ms": m.get("period_ms"), "weight": m.get("weight", 1.0)}
        if self.o.sync_target_ms > 0:
            req["target_ms"] = self.o.sync_target_ms
        if req["slack_ms"] is None or req["period_ms"] is None:
            return
        self._udp.sendto(json.dumps(req, separators=(",", ":")).encode(), self._driver)

    def _accept(self, srv: socket.socket) -> None:
        while True:
            conn, addr = srv.accept()
            self.log(f"headset connected over USB {addr}")
            self._thread(self._serve_client, conn)

    def _serve_client(self, conn: socket.socket) -> None:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.link.via_tcp(conn)
        buf = b""
        with conn:
            while True:
                try:
                    data = conn.recv(65536)
                except OSError:
                    break
                if not data:
                    break
                buf += data
                *lines, buf = buf.split(b"\n")
                for line in lines:
                    if line:
                        self._forward(line)

    def _serve_udp(self, sock: socket.socket) -> None:
        """Poses over the network: one JSON array per datagram."""
        peer = None
        while True:
            try:
                data, addr = sock.recvfrom(65536)
            except OSError:
                continue   # Windows: ICMP "port unreachable" from an earlier send
            if addr != peer:
                peer = addr
                self.log(f"headset sending over the network from {addr[0]}")
                self.link.via_udp(sock, addr)
            self._forward(data)

    def _serve_discovery(self, sock: socket.socket) -> None:
        """Answers the app's broadcast with this PC's ports (unicast)."""
        reply = json.dumps({"vhmd": "pc", "name": platform.node(), "pose_port": self.o.port,
                            "video_port": VIDEO_PORT}).encode()
        while True:
            try:
                data, addr = sock.recvfrom(1024)
            except OSError:
                continue
            if data.startswith(b"VHMD-DISCOVER"):
                sock.sendto(reply, addr)

    def _serve_driver_replies(self) -> None:
        """The driver answers the socket the poses come from: haptic events go on
        to the headset (other replies are dropped)."""
        while True:
            try:
                data, _ = self._udp.recvfrom(65536)
            except OSError:
                continue
            if b'"haptic"' in data:
                self.link.send(data)
            elif b'"seq"' in data:
                try:
                    reply = json.loads(data)
                except ValueError:
                    continue
                if reply.get("seq") == -1:   # display_sync, not waited for
                    self.sync_state = reply
                    self.sync_shift_total += reply.get("shift_ms", 0.0) or 0.0
                    continue
                with self._reply_cv:
                    self._replies[reply.get("seq")] = reply
                    while len(self._replies) > 32:   # unanswered leftovers
                        self._replies.pop(next(iter(self._replies)))
                    self._reply_cv.notify_all()

    # -- once a second ----------------------------------------------------------
    def _monitor(self) -> None:
        from . import mode
        last_steamvr_check = 0.0
        window = time.monotonic()
        while True:
            time.sleep(max(0.0, 1.0 - (time.monotonic() - window)))
            now = time.monotonic()
            dt, window = max(0.001, now - window), now
            n, b, last = self.stats.take()
            fin = fout = vb = 0
            if self.video:
                fin, fout, vb = self.video.take_stats()
            self.rates = {"msgs": round(n / dt), "kib": b / 1024 / dt, "video_in": round(fin / dt),
                          "video_out": round(fout / dt), "mbps": vb * 8 / 1e6 / dt}
            self.summary = summary(last) if n else ""
            fwd, self._fwd_samples = self._fwd_samples, []
            self.forward_us = (sum(fwd) / len(fwd), max(fwd)) if fwd else (0.0, 0.0)

            # SteamVR and the driver: is it up, who has control of the HMD.
            if now - last_steamvr_check > 3.0:
                last_steamvr_check = now
                try:
                    self.pc_state["steamvr"] = mode.steamvr_running()
                except Exception:
                    self.pc_state["steamvr"] = False
            try:
                c = self.request("control").get("control", {})
                self.pc_state["driver"] = True
                owner = c.get("devices", {}).get("hmd", c.get("owner", ""))
                self.pc_state["control"] = owner or "any"
            except (OSError, TimeoutError, ValueError):
                self.pc_state["driver"] = False
                self.pc_state["control"] = ""
                self._models_applied = None
            if self.pc_state["driver"]:
                self.pc_state["steamvr"] = True

            # Controllers as axes for the Dev edition, labelled shapes otherwise.
            edition = self.info.get("edition")
            if self.pc_state["driver"] and edition and self.connected():
                want = "axes" if edition == "dev" else "labeled"
                if self._models_applied != want:
                    try:
                        if not self.request("render_models", mode=want).get("ok"):
                            raise ValueError("render_models refused")
                        self._models_applied = want
                        self.log(f"controllers shown as {want} (VHMD Link {edition} edition)")
                    except (OSError, TimeoutError, ValueError):
                        pass

            # VHMD renders at the headset's refresh rate (changed live in direct mode).
            hs_rate = float(self.info.get("refresh") or 0)
            if self.pc_state["driver"] and self.connected() and hs_rate > 0:
                try:
                    drv_rate = float(self.request("refresh_rate").get("hz") or 0)
                    if drv_rate and abs(drv_rate - hs_rate) > 0.5:
                        r = self.request("refresh_rate", hz=hs_rate)
                        note = (f"VHMD now renders at {hs_rate:g} Hz like the headset" if r.get("ok") else
                                f"VHMD stays at {drv_rate:g} Hz (headset {hs_rate:g} Hz): {r.get('error', '')}")
                        if note != self._rate_note:
                            self._rate_note = note
                            self.log(note)
                except (OSError, TimeoutError, ValueError):
                    pass

            if self.connected():
                status = {"type": "status", "pc": platform.node(), **self.pc_state}
                if self.o.refresh > 0:
                    status["refresh"] = self.o.refresh
                self.link.send(json.dumps(status, separators=(",", ":")).encode())


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


def report_line(st: dict) -> str:
    line = f"{st['msgs']:4d} msg/s  {st['kib']:6.1f} KiB/s  {st['summary']}" if st["msgs"] else ""
    if st["video_in"] or st["video_out"]:
        r = st["relay_ms"]
        line += (f"  |  video in {st['video_in']} / out {st['video_out']} fps  {st['mbps']:5.1f} Mbit/s"
                 f"  relay {r[0]:.2f}/{r[1]:.2f}/{r[2]:.2f} ms  pose fwd {st['forward_us'][0]:.0f}/{st['forward_us'][1]:.0f} us")
    if st["sync"]:
        line += f"  |  sync {st['sync'].get('shift_ms', 0):+.2f} ms (total {st['sync_total_ms']:+.1f})"
        if not st["sync"].get("ok", True):
            line += f" {st['sync'].get('error', '')}"
    h = st["headset_stats"]
    if h:
        line += (f"  |  headset: received {h.get('received', 0):.0f} decoded {h.get('decoded', 0):.0f} "
                 f"shown {h.get('shown', 0):.0f}/s at {h.get('display', 0):.0f} Hz "
                 f"(repeats {h.get('repeats', 0):.0f}/s, buffer {h.get('buffer', 0)}, "
                 f"wait {h.get('wait_ms', 0):.1f} ms), {h.get('reprojected', 0):.0f}% reprojected")
    return line


def main() -> None:
    ap = argparse.ArgumentParser(description="Relay the VHMD Link headset app to the VHMD driver")
    ap.add_argument("--driver-host", default=DEFAULT_HOST)
    ap.add_argument("--driver-port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--port", type=int, default=LISTEN_PORT, help="TCP port the app connects to")
    ap.add_argument("--no-adb", action="store_true", help="do not run adb reverse")
    ap.add_argument("--usb-only", action="store_true", help="listen on 127.0.0.1 only (adb reverse), no network")
    ap.add_argument("--no-video", action="store_true", help="poses only, no video")
    ap.add_argument("--bitrate", type=float, default=40.0, help="video bitrate, Mbit/s")
    ap.add_argument("--codec", default="hevc", choices=("hevc", "h264", "av1"),
                    help="codec of the driver's encoder (av1: newer GPUs; falls back to hevc)")
    ap.add_argument("--eye-width", type=int, default=0, help="video width per eye (default: the headset's)")
    ap.add_argument("--video-source", default="auto", choices=("auto", "driver", "ffmpeg"),
                    help="auto: the driver's hardware encoder if it runs, else ffmpeg on the preview")
    ap.add_argument("--encoder", default="h264_amf", help="ffmpeg fallback: H.264 encoder (h264_amf, h264_nvenc, ...)")
    ap.add_argument("--buffer", type=int, default=0, choices=(0, 1, 2),
                    help="frames the headset holds back: 0 = newest (lowest latency), 1 = smoother (+1 frame)")
    ap.add_argument("--fixed-bitrate", action="store_true", help="keep --bitrate even when the network is slower")
    ap.add_argument("--refresh", type=float, default=0.0,
                    help="display refresh rate for the headset, e.g. 72 or 90 (VHMD follows it)")
    ap.add_argument("--no-sync", action="store_true", help="do not move VHMD's vsync to the headset's display")
    ap.add_argument("--sync-target-ms", type=float, default=0.0,
                    help="how long frames should wait on the headset before shown (default: 0.4 frame)")
    args = ap.parse_args()

    bridge = Bridge(BridgeOptions(
        driver_host=args.driver_host, driver_port=args.driver_port, port=args.port, adb=not args.no_adb,
        usb_only=args.usb_only, video=not args.no_video, bitrate=args.bitrate, codec=args.codec,
        eye_width=args.eye_width, video_source=args.video_source, encoder=args.encoder, buffer=args.buffer,
        adaptive=not args.fixed_bitrate, sync=not args.no_sync, sync_target_ms=args.sync_target_ms,
        refresh=args.refresh),
        log=lambda m: print(m, flush=True))
    bridge.start()
    try:
        while True:
            time.sleep(1.0)
            line = report_line(bridge.status())
            if line:
                print(line, flush=True)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
