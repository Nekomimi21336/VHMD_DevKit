"""Record what drives VHMD's devices and send it again at the same pace.

The driver records (config op "record"): every JSON datagram that moved a
device, from any sender (VHMD Link, a script, the keyboard/mouse tool), one line
each with its arrival time, into the VHMD data folder's recordings
(%LOCALAPPDATA%\\VHMD\\recordings, ~/.local/state/vhmd/recordings on Linux).
VMT's OSC and the microphone's sound are left out.

    python -m vhmd_client.replay record walk          # until Enter (or --seconds)
    python -m vhmd_client.replay play walk            # same pace; --loop, --speed 0.5
    python -m vhmd_client.replay play walk --take     # takes control first, gives it back after
    python -m vhmd_client.replay list

While a recording plays, a headset (or any sender) that keeps sending moves the
same devices; --take gives the replay control of the devices it moves ("src":
"replay"), so the others are ignored until it ends.
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import threading
import time
from pathlib import Path
from typing import Callable, Iterable, List, Optional, Set, Tuple

from .client import DEFAULT_HOST, DEFAULT_PORT, VHMDClient
from .paths import data_dir

FOLDER = data_dir() / "recordings"
SOURCE = "replay"


def path_of(name: str) -> Path:
    """A recording by name (in the recordings folder) or by path."""
    p = Path(name)
    if p.suffix != ".jsonl" and not p.exists():
        p = p.with_name(p.name + ".jsonl")
    return p if p.exists() or p.is_absolute() or len(p.parts) > 1 else FOLDER / p.name


def load(path: Path) -> List[Tuple[float, object]]:
    """(seconds, datagram as JSON) per line; broken lines are skipped."""
    out = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                rec = json.loads(line)
                out.append((float(rec["t"]), rec["d"]))
            except (ValueError, KeyError, TypeError):
                continue
    return out


def devices_in(frames: Iterable[Tuple[float, object]]) -> Set[str]:
    ids = set()
    for _, d in frames:
        for m in d if isinstance(d, list) else [d]:
            if isinstance(m, dict) and m.get("id"):
                ids.add(m["id"])
    return ids


def restamp(d, src: Optional[str]):
    """The datagram's device messages with "src" set (None: as recorded) and the
    sender's "ts" dropped (it was the time of the recording). Requests ("config")
    that came in the same datagram are not sent again; None if nothing is left."""
    def one(m):
        m = {k: v for k, v in m.items() if k != "ts"}
        if src is not None:
            m["src"] = src
        return m
    msgs = [one(m) for m in (d if isinstance(d, list) else [d])
            if isinstance(m, dict) and m.get("type") not in ("config", "mic")]
    if not msgs:
        return None
    return msgs if isinstance(d, list) else msgs[0]


class Player:
    """Sends a recording to the driver at its pace, on its own thread."""

    def __init__(self, frames: List[Tuple[float, object]], host: str = DEFAULT_HOST, port: int = DEFAULT_PORT,
                 speed: float = 1.0, loop: bool = False, src: Optional[str] = SOURCE,
                 on_end: Optional[Callable[[], None]] = None) -> None:
        self.frames, self.addr, self.speed, self.loop, self.src = frames, (host, port), max(0.05, speed), loop, src
        self.on_end = on_end
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.stop_event = threading.Event()
        self.sent = 0
        self.position = 0.0   # seconds into the recording
        self.thread = threading.Thread(target=self._run, daemon=True)

    def start(self) -> "Player":
        self.thread.start()
        return self

    def stop(self) -> None:
        self.stop_event.set()

    def wait(self) -> None:
        while self.thread.is_alive():
            self.thread.join(0.2)

    def duration(self) -> float:
        return self.frames[-1][0] if self.frames else 0.0

    def _run(self) -> None:
        try:
            while not self.stop_event.is_set():
                t0 = time.perf_counter()
                for t, d in self.frames:
                    due = t0 + t / self.speed
                    while not self.stop_event.is_set():
                        left = due - time.perf_counter()
                        if left <= 0:
                            break
                        time.sleep(min(left, 0.01) if left > 0.002 else 0)
                    if self.stop_event.is_set():
                        break
                    self.position = t
                    out = restamp(d, self.src)
                    if out is None:
                        continue
                    self.sock.sendto(json.dumps(out, separators=(",", ":")).encode(), self.addr)
                    self.sent += 1
                if not self.loop:
                    break
        finally:
            self.sock.close()
            if self.on_end:
                self.on_end()


def recordings() -> List[Path]:
    return sorted(FOLDER.glob("*.jsonl"), key=lambda p: p.stat().st_mtime, reverse=True)


def main(argv: Optional[List[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default=DEFAULT_HOST)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub = ap.add_subparsers(dest="cmd", required=True)
    rec = sub.add_parser("record", help="record what drives the devices")
    rec.add_argument("name", nargs="?", default="", help="file name (default: from the time)")
    rec.add_argument("--seconds", type=float, default=0, help="stop after this long (default: on Enter)")
    play = sub.add_parser("play", help="send a recording again")
    play.add_argument("name")
    play.add_argument("--speed", type=float, default=1.0)
    play.add_argument("--loop", action="store_true")
    play.add_argument("--take", action="store_true", help="take control of its devices, give it back after")
    play.add_argument("--as-recorded", action="store_true", help="keep the senders' names (src) as recorded")
    sub.add_parser("list", help="the recordings")
    args = ap.parse_args(argv)

    if args.cmd == "list":
        for p in recordings():
            frames = load(p)
            print(f"{p.stem:32s} {frames[-1][0] if frames else 0:7.1f} s  {len(frames):6d} datagrams  "
                  f"{', '.join(sorted(devices_in(frames)))}")
        print(f"({FOLDER})")
        return 0

    with VHMDClient(args.host, args.port, timeout=2.0) as c:
        if args.cmd == "record":
            r = c.request("record", mode="start", name=args.name)
            if not r.get("ok"):
                print("could not record:", r.get("error"), file=sys.stderr)
                return 1
            print("recording to", r["record"]["path"], "- Enter stops" if not args.seconds else "")
            try:
                time.sleep(args.seconds) if args.seconds else input()
            except KeyboardInterrupt:
                pass
            r = c.request("record", mode="stop")["record"]
            print(f"{r['lines']} datagrams, {r['seconds']:.1f} s: {r.get('path')}")
            return 0

        path = path_of(args.name)
        frames = load(path)
        if not frames:
            print("nothing to play in", path, file=sys.stderr)
            return 1
        ids = sorted(devices_in(frames))
        before = c.control()["control"]
        if args.take:
            c.control(SOURCE, ids)
        else:
            held = [f"{i} ({o})" for i in ids
                    for o in [before.get("devices", {}).get(i, before.get("owner", ""))] if o not in ("", SOURCE)]
            if held and not args.as_recorded:
                print("controlled by another sender, so ignored by the driver: " + ", ".join(held) +
                      " (--take gives them to the replay)", file=sys.stderr)
            before = None
        player = Player(frames, args.host, args.port, args.speed, args.loop,
                        None if args.as_recorded else SOURCE).start()
        print(f"playing {path.name}: {player.duration():.1f} s, {len(frames)} datagrams, devices {', '.join(ids)}"
              + ("  (Ctrl+C stops)" if args.loop else ""))
        try:
            player.wait()
        except KeyboardInterrupt:
            player.stop()
            player.wait()
        if before is not None:   # back to whoever had them
            owners = before.get("devices", {})
            default = before.get("owner", "")
            for i in ids:
                c.control(owners.get(i, default), [i])
        print(f"sent {player.sent} datagrams")
    return 0


if __name__ == "__main__":
    sys.exit(main())
