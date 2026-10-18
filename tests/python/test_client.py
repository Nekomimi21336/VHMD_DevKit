"""Unit tests for clients/python/vhmd_client (no SteamVR needed).

    python -m pytest tests/python
"""

import json
import math
import socket
import sys
import threading
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "clients" / "python"))

from vhmd_client import VHMDClient, math3d  # noqa: E402


@pytest.fixture
def server():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    s.settimeout(2.0)
    yield s
    s.close()


def recv_json(s):
    data, addr = s.recvfrom(65536)
    return json.loads(data), addr


def test_yaw_turns_forward_to_left():
    v = math3d.rotate(math3d.from_euler(yaw=90), (0, 0, -1))
    assert v == pytest.approx((-1, 0, 0), abs=1e-9)


def test_matrix_roundtrip():
    q = math3d.from_euler(30, -20, 10)
    w, x, y, z = q
    m = [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0.1],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 1.2],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), -0.3],
    ]
    pos, q2 = math3d.from_matrix34(m)
    assert pos == pytest.approx((0.1, 1.2, -0.3))
    assert math.degrees(math3d.angle_between(q, q2)) < 1e-6


def test_pose_message_format(server):
    with VHMDClient(port=server.getsockname()[1]) as c:
        c.pose("hmd", (0, 1.7, 0), (1, 0, 0, 0), vel=(0, 0, 0))
        msg, _ = recv_json(server)
    assert msg["type"] == "pose" and msg["id"] == "hmd"
    assert msg["pos"] == [0, 1.7, 0] and msg["rot"] == [1, 0, 0, 0]
    assert "ts" in msg and "angvel" not in msg


def test_batch_is_one_datagram(server):
    with VHMDClient(port=server.getsockname()[1]) as c:
        with c.batch():
            c.pose("hmd", (0, 1.7, 0))
            c.inputs("right_hand", {"trigger": 0.5, "a": True})
        msg, _ = recv_json(server)
    assert isinstance(msg, list) and len(msg) == 2
    assert msg[1]["values"] == {"trigger": 0.5, "a": True}


def test_send_many_splits_at_256(server):
    with VHMDClient(port=server.getsockname()[1]) as c:
        c.send_many([{"type": "config", "op": "ping"}] * 300)
        first, _ = recv_json(server)
        second, _ = recv_json(server)
    assert len(first) == 256 and len(second) == 44


def test_skeleton_validation():
    with VHMDClient(port=9) as c:
        with pytest.raises(ValueError):
            c.skeleton_bones("left_hand", [(1, 0, 0, 0)] * 30)


def test_request_matches_seq_and_skips_unrelated(server):
    def fake_driver():
        msg, addr = recv_json(server)
        server.sendto(json.dumps({"type": "haptic", "id": "right_hand"}).encode(), addr)  # unrelated
        server.sendto(json.dumps({"type": "result", "seq": msg["seq"], "ok": True}).encode(), addr)

    t = threading.Thread(target=fake_driver)
    t.start()
    with VHMDClient(port=server.getsockname()[1]) as c:
        assert c.ping() is True
    t.join()


def test_hand_mode(server):
    seen = []

    def fake_driver():
        for reply in ({"ok": True, "mode": "hands"}, {"ok": False, "error": "bad mode", "mode": "hands"}):
            msg, addr = recv_json(server)
            seen.append(msg)
            server.sendto(json.dumps({"type": "result", "seq": msg["seq"], **reply}).encode(), addr)

    t = threading.Thread(target=fake_driver)
    t.start()
    with VHMDClient(port=server.getsockname()[1]) as c:
        assert c.hand_mode("hands") == "hands"
        with pytest.raises(ValueError):
            c.hand_mode("feet")
    t.join()
    assert seen[0]["op"] == "hand_mode" and seen[0]["mode"] == "hands"


def test_request_timeout():
    with VHMDClient(port=9, timeout=0.2) as c:  # nothing listens on the discard port
        assert c.ping() is False
