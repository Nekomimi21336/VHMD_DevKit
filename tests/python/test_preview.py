"""Tests for vhmd_client.preview against a file written with the driver's layout."""

import struct

import pytest
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "clients" / "python"))

from vhmd_client.preview import PreviewReader  # noqa: E402

# Mirrors driver/src/util/preview_shm.h (72-byte header).
HDR = struct.Struct("<8s6I3Q2I2I")


def write_preview(path, w, h, seq=2, frame_index=7, left=(255, 0, 0, 255), right=(0, 0, 255, 255)):
    eye = w // 2
    rows = []
    for _ in range(h):
        rows.append(bytes(left) * eye + bytes(right) * eye)
    pixels = b"".join(rows)
    hdr = HDR.pack(b"VHMDPRV1", 1, 72, w, h, w * 4, 1, seq, frame_index, 1_700_000_000_000_000_000, eye, 43, 0, 0)
    path.write_bytes(hdr + pixels)


def test_header_layout_matches_driver():
    assert HDR.size == 72
    assert struct.calcsize("<8s6I") == 32  # seq offset used by the reader


def test_read_and_crop(tmp_path):
    p = tmp_path / "prev"
    write_preview(p, 8, 4)
    with PreviewReader(str(p)) as r:
        f = r.read()
    assert f and (f.width, f.height, f.eye_width, f.frame_index, f.source_format) == (8, 4, 4, 7, 43)
    assert f.rgba[:4] == bytes((255, 0, 0, 255)) and f.rgba[16:20] == bytes((0, 0, 255, 255))
    left, right = f.eye("left"), f.eye("right")
    assert left.width == 4 and set(left.rgb()[0::3]) == {255} and set(right.rgb()[2::3]) == {255}
    assert f.mean_rgb()[0] > 100 and f.mean_rgb()[2] > 100


def test_torn_write_is_rejected(tmp_path):
    p = tmp_path / "prev"
    write_preview(p, 8, 4, seq=3)  # odd = writer in progress
    with PreviewReader(str(p)) as r:
        assert r.read(retries=3) is None


def test_missing_and_bad_magic(tmp_path):
    with PreviewReader(str(tmp_path / "nope")) as r:
        assert r.read() is None
    p = tmp_path / "bad"
    p.write_bytes(b"X" * 100)
    with PreviewReader(str(p)) as r:
        assert r.read() is None


def test_png_roundtrip(tmp_path):
    p = tmp_path / "prev"
    write_preview(p, 8, 4)
    with PreviewReader(str(p)) as r:
        f = r.read()
    out = tmp_path / "x.png"
    f.save_png(str(out))
    data = out.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    w, h = struct.unpack(">II", data[16:24])
    assert (w, h) == (8, 4)
    idat = data[data.index(b"IDAT") + 4:data.index(b"IEND") - 8]
    raw = zlib.decompress(idat)
    assert len(raw) == 4 * (1 + 8 * 4) and raw[1:5] == bytes((255, 0, 0, 255))


def test_read_scaled(tmp_path):
    p = tmp_path / "prev"
    write_preview(p, 16, 8, left=(10, 20, 30, 255), right=(200, 100, 50, 255))
    with PreviewReader(str(p)) as r:
        w, h, idx, rgb = r.read_scaled(4, 4)                       # 8x8 eye -> factor 2
        assert (w, h, idx) == (4, 4, 7) and len(rgb) == 4 * 4 * 3
        assert rgb[:3] == bytes((10, 20, 30))
        w, h, _, bgrx = r.read_scaled(8, 8, eye="right", pixel_format="bgrx")
        assert (w, h) == (8, 8) and bgrx[:3] == bytes((50, 100, 200))   # blue, green, red
        assert r.read_scaled(4, 4, skip_index=7) is None              # unchanged frame


def test_read_scaled_both_eyes_and_aspect(tmp_path):
    p = tmp_path / "prev"
    write_preview(p, 32, 18, left=(10, 20, 30, 255), right=(200, 100, 50, 255))   # 16x18 eyes
    with PreviewReader(str(p)) as r:
        w, h, _, rgb = r.read_scaled(32, 18, eye="both")
        assert (w, h) == (32, 18)
        assert rgb[:3] == bytes((10, 20, 30)) and rgb[(w - 1) * 3:w * 3] == bytes((200, 100, 50))
        w, h, _, rgb = r.read_scaled(16, 18, aspect=16 / 9)       # 16x18 eye -> 16x9 center
        assert (w, h) == (16, 9)
        w, h, _, rgb = r.read_scaled(8, 8, eye="both", aspect=1.0)  # 32x18 -> 18x18 -> /3
        assert (w, h) == (6, 6)
        assert rgb[:3] == bytes((10, 20, 30)) and rgb[-3:] == bytes((200, 100, 50))


def test_render_pose_after_the_original_header(tmp_path):
    """Newer drivers append pose_flags + pose[7] and grow header_size to 104."""
    w, h = 8, 4
    pixels = bytes((10, 20, 30, 255)) * (w * h)
    hdr = HDR.pack(b"VHMDPRV1", 1, 104, w, h, w * 4, 1, 2, 5, 1_700_000_000_000_000_000, w // 2, 28, 0, 0)
    pose = struct.pack("<I7f", 1, 0.1, 1.6, -0.2, 1.0, 0.0, 0.0, 0.0)
    p = tmp_path / "prev"
    p.write_bytes(hdr + pose + pixels)
    with PreviewReader(str(p)) as r:
        f = r.read()
        assert f.rgba[:4] == bytes((10, 20, 30, 255))           # pixels start at header_size
        assert f.pose == pytest.approx((0.1, 1.6, -0.2, 1.0, 0.0, 0.0, 0.0))
    p.write_bytes(hdr + struct.pack("<I7f", 0, *[0.0] * 7) + pixels)
    with PreviewReader(str(p)) as r:
        assert r.read().pose is None                             # flag clear: no pose
    write_preview(p, 8, 4)
    with PreviewReader(str(p)) as r:
        assert r.read().pose is None                             # 72-byte header: no pose


def test_headset_display_settings():
    import math
    from vhmd_client import headset_info
    info = {"render": [1920, 1920], "refresh": 72.0,
            "fov": [[-0.83, 0.80, 0.82, -0.84], [-0.80, 0.83, 0.82, -0.84]],
            "eyes": [[-0.036, 0, 0, 1, 0, 0, 0], [0.036, 0, 0, 1, 0, 0, 0]]}
    s = headset_info.display_settings(info)
    assert s["renderWidth"] == 1920 and s["displayFrequency"] == 72.0
    assert s["ipdMeters"] == pytest.approx(0.072)
    assert s["leftTanLeft"] == pytest.approx(math.tan(-0.83)) and s["rightTanRight"] == pytest.approx(math.tan(0.83))
    assert headset_info.differs(info, [[-0.8726] * 4, [-0.8726] * 4])
    assert not headset_info.differs(info, info["fov"])
