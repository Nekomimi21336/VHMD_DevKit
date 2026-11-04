"""Recording files and their replay (no SteamVR needed)."""

import json
import socket
import sys
import time
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "clients" / "python"))

from vhmd_client import replay  # noqa: E402


def write_recording(path: Path) -> None:
    lines = [
        {"t": 0.0, "d": {"type": "pose", "id": "hmd", "pos": [0, 1.6, 0], "rot": [1, 0, 0, 0], "src": "manual", "ts": 1.0}},
        {"t": 0.1, "d": [{"type": "input", "id": "left_hand", "values": {"trigger": 1}, "src": "manual"},
                         {"type": "config", "op": "display_sync", "slack_ms": 4}]},
        {"t": 0.2, "d": {"type": "config", "op": "list"}},
    ]
    path.write_text("\n".join(json.dumps(x) for x in lines) + "\n{broken\n", encoding="utf-8")


def test_load_and_devices(tmp_path):
    p = tmp_path / "walk.jsonl"
    write_recording(p)
    frames = replay.load(p)
    assert [t for t, _ in frames] == [0.0, 0.1, 0.2]   # the broken line is skipped
    assert replay.devices_in(frames) == {"hmd", "left_hand"}


def test_restamp_sets_source_and_drops_requests():
    one = replay.restamp({"type": "pose", "id": "hmd", "src": "manual", "ts": 5.0}, "replay")
    assert one == {"type": "pose", "id": "hmd", "src": "replay"}
    batch = replay.restamp([{"type": "input", "id": "left_hand", "src": "manual"},
                            {"type": "config", "op": "display_sync"}], None)
    assert batch == [{"type": "input", "id": "left_hand", "src": "manual"}]
    assert replay.restamp({"type": "config", "op": "list"}, "replay") is None
    assert replay.restamp({"type": "mic", "seq": 1}, "replay") is None


def test_path_of_names_and_paths(tmp_path, monkeypatch):
    monkeypatch.setattr(replay, "FOLDER", tmp_path)
    assert replay.path_of("walk") == tmp_path / "walk.jsonl"
    other = tmp_path / "sub" / "x.jsonl"
    other.parent.mkdir()
    other.write_text("")
    assert replay.path_of(str(other)) == other


def test_player_keeps_the_pace(tmp_path):
    server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    server.bind(("127.0.0.1", 0))
    server.settimeout(2.0)
    p = tmp_path / "walk.jsonl"
    write_recording(p)
    t0 = time.perf_counter()
    player = replay.Player(replay.load(p), port=server.getsockname()[1], speed=0.5).start()
    got = []
    for _ in range(2):   # the third line is only a request: not sent
        data, _ = server.recvfrom(65536)
        got.append((time.perf_counter() - t0, json.loads(data)))
    player.wait()
    server.close()
    assert player.sent == 2
    assert got[0][1]["src"] == "replay" and "ts" not in got[0][1]
    assert got[1][1] == [{"type": "input", "id": "left_hand", "values": {"trigger": 1}, "src": "replay"}]
    assert got[1][0] - got[0][0] == pytest.approx(0.2, abs=0.05)   # 0.1 s recorded, at half speed
