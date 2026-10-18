"""Generates driver/resources/rendermodels/vhmd_controller/: a simple original controller mesh plus the
component poses ("tip", "handgrip", "base", ...) that apps use to place pointers and held objects.

Without components (SteamVR's generic_controller is a single mesh) some apps cannot find where the
controller points; VRChat, for one, then never hits its menu buttons with the laser.

    python tools/gen_rendermodel.py

Coordinates are in the controller's raw pose space: +X right, +Y up, -Z forward (pointing direction).
"""

from __future__ import annotations

import json
import math
import struct
import zlib
from pathlib import Path

NAME = "vhmd_controller"
OUT = Path(__file__).resolve().parents[1] / "driver" / "resources" / "rendermodels" / NAME


def capsule_mesh(radius=0.018, half_len=0.06, rings=8, segments=16):
    """Capsule along Z, centered a little behind the pose origin (handle in the hand, tip forward)."""
    verts, norms, uvs, faces = [], [], [], []
    z_center = 0.02
    rows = []
    for i in range(rings * 2 + 2):
        if i <= rings:  # front hemisphere (-Z)
            a = math.pi / 2 * (1 - i / rings)
            zc, r, nz = z_center - half_len - radius * math.sin(a) + radius * 0, radius * math.cos(a), -math.sin(a)
            zc = z_center - half_len - radius * math.sin(a)
        else:  # back hemisphere (+Z)
            k = i - rings - 1
            a = math.pi / 2 * (k / rings)
            zc, r, nz = z_center + half_len + radius * math.sin(a), radius * math.cos(a), math.sin(a)
        row = []
        for j in range(segments + 1):
            t = 2 * math.pi * j / segments
            x, y = r * math.cos(t), r * math.sin(t)
            nr = math.sqrt(max(0.0, 1 - nz * nz))
            verts.append((x, y, zc))
            norms.append((nr * math.cos(t), nr * math.sin(t), nz))
            uvs.append((j / segments, i / (rings * 2 + 1)))
            row.append(len(verts))
        rows.append(row)
    for a, b in zip(rows, rows[1:]):
        for j in range(segments):
            faces.append((a[j], b[j], b[j + 1]))
            faces.append((a[j], b[j + 1], a[j + 1]))
    return verts, norms, uvs, faces


def write_obj(path: Path, mtl_name: str):
    v, n, t, f = capsule_mesh()
    lines = [f"mtllib {mtl_name}.mtl", f"usemtl {mtl_name}"]
    lines += [f"v {x:.5f} {y:.5f} {z:.5f}" for x, y, z in v]
    lines += [f"vt {u:.4f} {w:.4f}" for u, w in t]
    lines += [f"vn {x:.4f} {y:.4f} {z:.4f}" for x, y, z in n]
    lines += [f"f {a}/{a}/{a} {b}/{b}/{b} {c}/{c}/{c}" for a, b, c in f]
    path.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")


def write_png(path: Path, size=16, rgb=(70, 110, 140)):
    raw = b"".join(b"\x00" + bytes(rgb) * size for _ in range(size))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    write_obj(OUT / "body.obj", "body")
    (OUT / "body.mtl").write_text("newmtl body\nKa 1 1 1\nKd 1 1 1\nmap_Kd body.png\n", encoding="ascii", newline="\n")
    write_png(OUT / "body.png")
    no_rot = [0.0, 0.0, 0.0]
    model = {
        "thumbnail": "body.png",
        "components": {
            "body": {"filename": "body.obj", "component_local": {"origin": [0.0, 0.0, 0.0], "rotate_xyz": no_rot}},
            # Where the controller points (laser origin): front end of the capsule.
            "tip": {"component_local": {"origin": [0.0, 0.0, -0.06], "rotate_xyz": no_rot}},
            # Where an object held in the hand sits.
            "handgrip": {"component_local": {"origin": [0.0, 0.0, 0.03], "rotate_xyz": no_rot}},
            "base": {"component_local": {"origin": [0.0, -0.02, 0.10], "rotate_xyz": [0.0, 180.0, 0.0]}},
            "openxr_grip": {"component_local": {"origin": [0.0, 0.0, 0.03], "rotate_xyz": no_rot}},
            "openxr_aim": {"component_local": {"origin": [0.0, 0.0, -0.06], "rotate_xyz": no_rot}},
        },
    }
    (OUT / f"{NAME}.json").write_text(json.dumps(model, indent=2) + "\n", encoding="ascii", newline="\n")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
