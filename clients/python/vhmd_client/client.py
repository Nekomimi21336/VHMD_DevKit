"""UDP/JSON client for the VirtualHMD SteamVR driver. See docs/protocol.md."""

from __future__ import annotations

import json
import socket
import time
from typing import Any, Dict, Iterable, List, Mapping, Optional, Sequence

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 39570


class VHMDClient:
    """Sends pose / input / skeleton / gaze messages to the driver.

    Messages can be sent one per datagram, or collected with :meth:`batch`
    and flushed as a single JSON array (one datagram, applied together).

    >>> with VHMDClient() as c:
    ...     c.pose("hmd", (0, 1.7, 0), (1, 0, 0, 0))
    """

    MAX_DATAGRAM = 60000

    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT, timeout: float = 1.0):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0" if host not in ("127.0.0.1", "localhost") else "127.0.0.1", 0))
        self.sock.settimeout(timeout)
        self._batch: Optional[List[Dict[str, Any]]] = None
        self._seq = 0

    # -- plumbing -----------------------------------------------------------
    def close(self) -> None:
        self.sock.close()

    def __enter__(self) -> "VHMDClient":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def send(self, msg: Mapping[str, Any]) -> None:
        """Send (or queue, inside :meth:`batch`) one raw protocol message."""
        if self._batch is not None:
            self._batch.append(dict(msg))
        else:
            self.sock.sendto(json.dumps(msg, separators=(",", ":")).encode(), self.addr)

    def send_many(self, msgs: Iterable[Mapping[str, Any]]) -> None:
        """Send messages as JSON arrays, split to fit in UDP datagrams."""
        chunk: List[Mapping[str, Any]] = []
        size = 2
        for m in msgs:
            enc = len(json.dumps(m, separators=(",", ":"))) + 1
            if chunk and (size + enc > self.MAX_DATAGRAM or len(chunk) >= 256):
                self.sock.sendto(json.dumps(chunk, separators=(",", ":")).encode(), self.addr)
                chunk, size = [], 2
            chunk.append(m)
            size += enc
        if chunk:
            self.sock.sendto(json.dumps(chunk, separators=(",", ":")).encode(), self.addr)

    class _Batch:
        def __init__(self, client: "VHMDClient"):
            self.client = client

        def __enter__(self):
            self.client._batch = []
            return self.client

        def __exit__(self, exc_type, *rest):
            msgs, self.client._batch = self.client._batch, None
            if exc_type is None and msgs:
                self.client.send_many(msgs)

    def batch(self) -> "VHMDClient._Batch":
        """Context manager: messages sent inside are flushed as one array."""
        return VHMDClient._Batch(self)

    # -- messages -----------------------------------------------------------
    def pose(
        self,
        device_id: str,
        pos: Sequence[float],
        rot: Sequence[float] = (1.0, 0.0, 0.0, 0.0),
        vel: Optional[Sequence[float]] = None,
        angvel: Optional[Sequence[float]] = None,
        ts: Optional[float] = None,
        acc: Optional[Sequence[float]] = None,
        angacc: Optional[Sequence[float]] = None,
        age: Optional[float] = None,
    ) -> None:
        """Pose in meters / quaternion (w, x, y, z), SteamVR space.

        acc / angacc: acceleration (m/s^2, rad/s^2). age: how old the sample
        already is (seconds, 0..0.5); SteamVR then predicts from when it was taken.
        """
        msg: Dict[str, Any] = {"type": "pose", "id": device_id, "pos": list(pos), "rot": list(rot)}
        if vel is not None:
            msg["vel"] = list(vel)
        if angvel is not None:
            msg["angvel"] = list(angvel)
        if acc is not None:
            msg["acc"] = list(acc)
        if angacc is not None:
            msg["angacc"] = list(angacc)
        if age is not None:
            msg["age"] = age
        msg["ts"] = time.time() if ts is None else ts
        self.send(msg)

    def invalidate(self, device_id: str) -> None:
        """Report the device as not tracked (pose invalid) until the next pose."""
        self.send({"type": "pose", "id": device_id, "valid": False})

    def input(self, device_id: str, name: str, value: float | bool) -> None:
        self.send({"type": "input", "id": device_id, "name": name, "value": value})

    def proximity(self, worn: bool) -> None:
        """HMD proximity sensor: True = the headset is worn."""
        self.input("hmd", "proximity", worn)

    def battery(self, device_id: str, level: float, charging: Optional[bool] = None) -> None:
        """Battery level 0..1 (and charging state) shown by SteamVR."""
        values: Dict[str, float | bool] = {"battery": level}
        if charging is not None:
            values["charging"] = charging
        self.inputs(device_id, values)

    def inputs(self, device_id: str, values: Mapping[str, float | bool]) -> None:
        self.send({"type": "input", "id": device_id, "values": dict(values)})

    def skeleton_curl(
        self, device_id: str, curl: Sequence[float], splay: Optional[Sequence[float]] = None
    ) -> None:
        """Finger curl 0..1 and splay -1..1 for (thumb, index, middle, ring, pinky)."""
        msg: Dict[str, Any] = {"type": "skeleton", "id": device_id, "curl": list(curl)}
        if splay is not None:
            msg["splay"] = list(splay)
        self.send(msg)

    def skeleton_bones(
        self,
        device_id: str,
        rotations: Sequence[Sequence[float]],
        positions: Optional[Sequence[Sequence[float]]] = None,
    ) -> None:
        """31 bone rotations (w,x,y,z) in OpenVR hand skeleton order (parent space)."""
        if len(rotations) != 31:
            raise ValueError("rotations must contain 31 quaternions")
        msg: Dict[str, Any] = {"type": "skeleton", "id": device_id, "bones": [list(q) for q in rotations]}
        if positions is not None:
            if len(positions) != 31:
                raise ValueError("positions must contain 31 vectors")
            msg["positions"] = [list(p) for p in positions]
        self.send(msg)

    def gaze(
        self,
        origin: Sequence[float] = (0.0, 0.0, 0.0),
        direction: Optional[Sequence[float]] = None,
        target: Optional[Sequence[float]] = None,
        valid: bool = True,
        device_id: str = "hmd",
    ) -> None:
        msg: Dict[str, Any] = {"type": "gaze", "id": device_id, "origin": list(origin), "valid": valid}
        if target is not None:
            msg["target"] = list(target)
        elif direction is not None:
            msg["dir"] = list(direction)
        self.send(msg)

    # -- requests with replies ----------------------------------------------
    def request(self, op: str, **args: Any) -> Dict[str, Any]:
        """Send a config request and wait for the matching reply."""
        self._seq += 1
        seq = self._seq
        msg = {"type": "config", "op": op, "seq": seq, **args}
        self.sock.sendto(json.dumps(msg).encode(), self.addr)
        deadline = time.monotonic() + (self.sock.gettimeout() or 1.0)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"no reply to '{op}' from {self.addr[0]}:{self.addr[1]} (is SteamVR running?)")
            self.sock.settimeout(remaining)
            data, _ = self.sock.recvfrom(65536)
            try:
                reply = json.loads(data)
            except ValueError:
                continue
            if reply.get("seq") == seq:
                return reply
            # Unrelated datagram (e.g. a haptic event): ignore and keep waiting.

    def status(self) -> Dict[str, Any]:
        return self.request("list")

    def ping(self) -> bool:
        try:
            return bool(self.request("ping").get("ok"))
        except (TimeoutError, OSError):
            return False

    def add(self, device_id: str = "") -> Dict[str, Any]:
        """Register an on-demand device (a tracker) with SteamVR now, without
        waiting for its first pose. No id = all on-demand devices. Sending a
        pose to a tracker adds it automatically, so this is optional."""
        return self.request("add", id=device_id) if device_id else self.request("add")

    def set_role(self, tracker_id: str, role: str) -> Dict[str, Any]:
        return self.request("set_role", id=tracker_id, role=role)

    def preview(self, on: bool = True) -> None:
        """Start (or pause) the Linux direct-mode preview in shared memory. The
        driver only captures frames while a tool has asked for them."""
        r = self.request("preview", mode="on" if on else "off")
        if not r.get("ok"):
            raise ValueError(r.get("error", "preview failed"))

    def refresh_rate(self, hz: float = 0) -> float:
        """Change the virtual display's frame rate while SteamVR runs (direct
        mode; 1-240). Without an argument, returns the current rate."""
        r = self.request("refresh_rate", hz=hz) if hz else self.request("refresh_rate")
        if not r.get("ok"):
            raise ValueError(r.get("error", "refresh_rate failed"))
        return float(r.get("hz", 0))

    def preview_window(self, on: bool = True) -> None:
        """Show (or hide) the preview window: the HMD image in a desktop window
        (Windows, display.mode = direct). Hidden costs nothing."""
        r = self.request("preview_window", mode="on" if on else "off")
        if not r.get("ok"):
            raise ValueError(r.get("error", "preview_window failed"))

    def hand_mode(self, mode: str = "") -> str:
        """Switch both hands between "controllers" and "hands" (hand tracking)
        while SteamVR runs, or return the current mode when called without
        one. The same ids ("left_hand", "right_hand") keep working in both."""
        r = self.request("hand_mode", mode=mode) if mode else self.request("hand_mode")
        if not r.get("ok"):
            raise ValueError(r.get("error", "hand_mode failed"))
        return r.get("mode", "")

    def reset(self, device_id: str = "") -> Dict[str, Any]:
        return self.request("reset", id=device_id) if device_id else self.request("reset")

    def poll_haptics(self, timeout: float = 0.0) -> List[Dict[str, Any]]:
        """Return haptic events the driver sent since the last call."""
        events = []
        old = self.sock.gettimeout()
        self.sock.settimeout(timeout if timeout > 0 else 0.000001)
        try:
            while True:
                data, _ = self.sock.recvfrom(65536)
                try:
                    msg = json.loads(data)
                except ValueError:
                    continue
                if msg.get("type") == "haptic":
                    events.append(msg)
        except (socket.timeout, BlockingIOError):
            pass
        finally:
            self.sock.settimeout(old)
        return events
