"""The native data path for VHMD Link (link/relay: vhmd-link-relay), steered
from Python.

headset_bridge.Bridge uses it when it is there (next to the exe in the PC
app, or link/target/release in the repository): poses, video, haptics and
discovery then never pass through Python, which only handles the headset's own
messages, the driver's answers and the settings. Without it the Bridge does
all of that itself (the same behaviour, more latency and jitter).

Preferred: the DLL (vhmdlinkrelay.dll) loaded into this process. Its threads
are native (no Python lock), and the network ports belong to this process,
which the firewall already knows; the standalone program would be a new one
to allow.

Control: UDP datagrams "KIND\\n" + payload on 127.0.0.1 (see link/relay/src/main.rs).
"""

from __future__ import annotations

import ctypes
import json
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Callable, Optional

EXE = "vhmd-link-relay.exe" if os.name == "nt" else "vhmd-link-relay"
DLL = "vhmdlinkrelay.dll" if os.name == "nt" else "libvhmdlinkrelay.so"


def find() -> Optional[Path]:
    """The relay, DLL first: $VHMD_LINK_RELAY, beside the frozen PC app, or the
    repository's release build."""
    env = os.environ.get("VHMD_LINK_RELAY")
    cands = [Path(env)] if env else []
    dirs = []
    if getattr(sys, "frozen", False):
        dirs += [Path(getattr(sys, "_MEIPASS", "")), Path(sys.executable).parent]
    dirs += [Path(__file__).resolve().parents[3] / "link" / "target" / "release"]
    cands += [d / DLL for d in dirs] + [d / EXE for d in dirs]
    for c in cands:
        if c.is_file():
            return c
    return None


_loaded: dict = {}   # DLL path -> control port (one relay per process)


class NativeRelay:
    """The relay process and its control socket. on_event(kind, payload) is
    called on a background thread for MSG / REPLY / STATS / LOG."""

    def __init__(self, path: Path, listen: str, name: str, on_event: Callable[[str, bytes], None]) -> None:
        self.on_event = on_event
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.proc = None
        self.kind = "dll" if path.suffix.lower() in (".dll", ".so") else "exe"
        if self.kind == "dll":
            key = str(path)
            if key not in _loaded:
                lib = ctypes.CDLL(key)
                start = lib.vhmd_link_relay_start
                start.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
                start.restype = ctypes.c_uint16
                err = ctypes.create_string_buffer(512)
                port = start(f"--listen\n{listen}\n--name\n{name}".encode(), err, len(err))
                if not port:
                    raise OSError(err.value.decode(errors="replace") or "vhmd-link-relay did not start")
                _loaded[key] = port
            self.addr = ("127.0.0.1", _loaded[key])
        else:
            args = [str(path), "--listen", listen, "--name", name]
            flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            self.proc = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                         stdin=subprocess.DEVNULL, creationflags=flags)
            line = self.proc.stdout.readline().decode(errors="replace").strip()   # "control 127.0.0.1:PORT"
            if not line.startswith("control "):
                err = self.proc.stderr.read().decode(errors="replace").strip() if self.proc.poll() is not None else line
                raise OSError(err or "vhmd-link-relay did not start")
            host, port = line.split()[1].rsplit(":", 1)
            self.addr = (host, int(port))
        self.stats: dict = {}
        self.stats_at = 0.0
        self._send(b"HELLO", b"")
        threading.Thread(target=self._events, daemon=True).start()
        threading.Thread(target=self._hello, daemon=True).start()

    def _send(self, kind: bytes, payload: bytes) -> None:
        try:
            self.sock.sendto(kind + b"\n" + payload, self.addr)
        except OSError:
            pass

    def _hello(self) -> None:
        while self.proc is None or self.proc.poll() is None:
            self._send(b"HELLO", b"")
            time.sleep(1.0)

    def _events(self) -> None:
        while True:
            try:
                data, _ = self.sock.recvfrom(1 << 17)
            except OSError:
                continue
            kind, _, payload = data.partition(b"\n")
            k = kind.decode(errors="replace")
            if k == "STATS":
                try:
                    self.stats = json.loads(payload)
                    self.stats_at = time.monotonic()
                except ValueError:
                    continue
            self.on_event(k, payload)

    def to_headset(self, data: bytes) -> None:
        self._send(b"TO_HEADSET", data)

    def driver(self, data: bytes) -> None:
        """To the driver, from the pose socket (the driver keeps sending haptics there)."""
        self._send(b"DRIVER", data)

    def video(self, settings: dict) -> None:
        self._send(b"VIDEO", json.dumps(settings, separators=(",", ":")).encode())

    def close(self) -> None:
        """The program ends; the DLL stays until this process does."""
        if self.proc is None:
            return
        self._send(b"QUIT", b"")
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.kill()


class NativeLink:
    """HeadsetLink's interface over the relay's view of the headset."""

    def __init__(self, relay: NativeRelay) -> None:
        self.relay = relay

    @property
    def last_seen(self) -> float:
        s = self.relay.stats
        if not s:
            return 0.0
        return self.relay.stats_at - float(s.get("last_seen_s", 1e9))

    def kind(self) -> Optional[str]:
        return self.relay.stats.get("link")

    def address(self) -> str:
        return self.relay.stats.get("address", "")

    def send(self, data: bytes) -> None:
        self.relay.to_headset(data)


class NativeVideo:
    """VideoServer's interface over the relay's video path."""

    def __init__(self, relay: NativeRelay, bitrate: float, codec: str, buffer: int, encoder_backend: str,
                 eye_width: int, eye_scale: float, adaptive: bool) -> None:
        self.relay = relay
        self.bitrate, self.codec, self.buffer = bitrate, codec, buffer
        self.encoder_backend, self.eye_width, self.eye_scale, self.adaptive = encoder_backend, eye_width, eye_scale, adaptive
        self._sent_eye = None
        self.apply(restart=False)

    def _eye(self) -> int:
        if self.eye_width:
            return self.eye_width
        from . import headset_info
        info = headset_info.load()
        eye = int(info["render"][0]) if info else 0
        if eye and self.eye_scale < 0.999:
            eye = max(640, int(eye * self.eye_scale) // 16 * 16)
        return eye

    def apply(self, restart: bool) -> None:
        eye = self._eye()
        self._sent_eye = eye
        self.relay.video({"bitrate_mbps": self.bitrate, "codec": self.codec, "buffer": self.buffer,
                          "encoder": self.encoder_backend, "eye_width": eye, "adaptive": self.adaptive,
                          "restart": restart})

    def headset_changed(self) -> None:
        """The headset described itself: a new eye width takes a reconnect."""
        if self._eye() != self._sent_eye:
            self.apply(restart=False)   # the relay reconnects when the eye width changes

    @property
    def current(self) -> bool:
        return bool(self.relay.stats.get("video_connected"))

    @property
    def bitrate_now(self) -> float:
        return float(self.relay.stats.get("bitrate_now", self.bitrate))

    @property
    def relay_ms(self):
        r = self.relay.stats.get("relay_ms") or [0.0, 0.0, 0.0]
        return tuple(r)

    def set_bitrate(self, mbps: float) -> None:
        self.bitrate = mbps
        self.relay.video({"bitrate_mbps": mbps})

    def restart(self) -> None:
        self.apply(restart=True)
