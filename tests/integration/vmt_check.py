"""VMT-compatible OSC check, run as its own process by run_integration.py.

Only runs when the driver has "vmt.enabled": true (status "vmt_enabled");
otherwise it reports itself as skipped. Sends VMT 0.15 OSC messages to the
driver port and checks what SteamVR shows: Room / Raw poses with a room
matrix, Joint / Follow relative to the HMD, Reset, and the /VMT/Out/...
messages (Alive, SubscribedDevice, Devices/List) on UDP 127.0.0.1:39571.
Prints one JSON line: {"results": [...], "info": {...}}.

    python tests/integration/vmt_check.py [--port 39570]
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "clients" / "python"))

import openvr  # noqa: E402

from vhmd_client import VHMDClient, math3d  # noqa: E402

OUT_PORT = 39571
TOL = 0.002


def osc(address: str, *args) -> bytes:
    def pad(b: bytes) -> bytes:
        return b + b"\0" * (4 - len(b) % 4)

    tags, body = ",", b""
    for a in args:
        if isinstance(a, str):
            tags += "s"
            body += pad(a.encode())
        elif isinstance(a, int):
            tags += "i"
            body += struct.pack(">i", a)
        else:
            tags += "f"
            body += struct.pack(">f", float(a))
    return pad(address.encode()) + pad(tags.encode()) + body


def osc_decode(data: bytes):
    def string(pos):
        end = data.index(b"\0", pos)
        return data[pos:end].decode(errors="replace"), (end + 4) & ~3

    address, pos = string(0)
    tags, pos = string(pos)
    args = []
    for t in tags[1:]:
        if t == "i":
            args.append(struct.unpack_from(">i", data, pos)[0])
            pos += 4
        elif t == "f":
            args.append(struct.unpack_from(">f", data, pos)[0])
            pos += 4
        elif t == "s":
            s, pos = string(pos)
            args.append(s)
    return address, args


def dist(a, b):
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=39570)
    args = ap.parse_args()

    results, info = [], {}

    def check(name, ok, detail=""):
        results.append({"name": name, "ok": bool(ok), "detail": detail})

    client = VHMDClient(port=args.port, timeout=2.0)
    status = client.status()
    if not status.get("vmt_enabled"):
        info["vmt_skipped"] = "vmt.enabled is false in vhmd_config.json"
        print(json.dumps({"results": results, "info": info}))
        return 0

    system = openvr.init(openvr.VRApplication_Background)
    send = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    out.bind(("127.0.0.1", OUT_PORT))
    out.settimeout(0.05)
    target = ("127.0.0.1", args.port)
    try:
        def vmt(address, *a):
            send.sendto(osc(address, *a), target)

        def index_of(serial):
            for i in range(openvr.k_unMaxTrackedDeviceCount):
                try:
                    if system.getStringTrackedDeviceProperty(i, openvr.Prop_SerialNumber_String) == serial:
                        return i
                except openvr.OpenVRError:
                    pass
            return None

        def pose(serial):
            i = index_of(serial)
            if i is None:
                return None
            p = system.getDeviceToAbsoluteTrackingPose(openvr.TrackingUniverseRawAndUncalibrated, 0.0, i + 1)[i]
            m = p.mDeviceToAbsoluteTracking
            return bool(p.bPoseIsValid), (m[0][3], m[1][3], m[2][3])

        def wait_pose(serial, want, resend, timeout=3.0):
            t0, last, got = time.perf_counter(), 0.0, None
            while time.perf_counter() - t0 < timeout:
                if time.perf_counter() - last > 0.05:
                    resend()
                    last = time.perf_counter()
                got = pose(serial)
                if got and got[0] and dist(got[1], want) < TOL:
                    return True, got
                time.sleep(0.01)
            return False, got

        def fmt(g):
            return "none" if g is None else f"valid={g[0]} pos={tuple(round(v, 3) for v in g[1])}"

        # The HMD as a known parent: (0, 1.6, 0), turned 90 degrees left.
        hmd_pos, hmd_rot = (0.0, 1.6, 0.0), math3d.from_euler(yaw=90)

        def hmd():
            client.pose("hmd", hmd_pos, hmd_rot)

        # 1. Room pose -> tracker_5 (on-demand add), identity room matrix.
        ok, got = wait_pose("VHMD-TRK-5", (0.3, 1.0, -0.2),
                            lambda: vmt("/VMT/Room/Driver", 5, 1, 0.0, 0.3, 1.0, -0.2, 0.0, 0.0, 0.0, 1.0))
        check("VMT /VMT/Room/Driver poses a tracker (added on demand)", ok, fmt(got))

        # 2. Room matrix: translate by (1, 0, 0); Room shifts, Raw does not.
        vmt("/VMT/SetRoomMatrix/Temporary", 1.0, 0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0)
        room_ok, room = wait_pose("VHMD-TRK-5", (1.3, 1.0, -0.2),
                                  lambda: vmt("/VMT/Room/Driver", 5, 1, 0.0, 0.3, 1.0, -0.2, 0.0, 0.0, 0.0, 1.0))
        raw_ok, raw = wait_pose("VHMD-TRK-5", (0.3, 1.0, -0.2),
                                lambda: vmt("/VMT/Raw/Driver", 5, 1, 0.0, 0.3, 1.0, -0.2, 0.0, 0.0, 0.0, 1.0))
        vmt("/VMT/SetRoomMatrix/Temporary", 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0)
        check("VMT room matrix moves Room poses but not Raw poses", room_ok and raw_ok,
              f"room {fmt(room)}, raw {fmt(raw)}")

        # 3. Joint / Follow relative to the HMD.
        def joint():
            hmd()
            vmt("/VMT/Joint/Driver", 6, 1, 0.0, 0.0, 0.0, -0.5, 0.0, 0.0, 0.0, 1.0, "HMD")

        def follow():
            hmd()
            vmt("/VMT/Follow/Driver", 7, 1, 0.0, 0.0, 0.0, -0.5, 0.0, 0.0, 0.0, 1.0, "HMD")

        offset = math3d.rotate(hmd_rot, (0.0, 0.0, -0.5))
        joint_ok, j = wait_pose("VHMD-TRK-6", tuple(h + o for h, o in zip(hmd_pos, offset)), joint)
        follow_ok, f = wait_pose("VHMD-TRK-7", (0.0, 1.6, -0.5), follow)
        check("VMT Joint follows the parent's position and rotation", joint_ok, fmt(j))
        check("VMT Follow follows the parent's position only", follow_ok, fmt(f))
        # The joint keeps following when only the parent moves.
        hmd_pos = (0.5, 1.6, 0.0)
        moved_ok, moved = wait_pose("VHMD-TRK-6", tuple(h + o for h, o in zip(hmd_pos, offset)), hmd)
        check("VMT Joint keeps following when only the parent moves", moved_ok, fmt(moved))

        # 4. Outputs on 127.0.0.1:39571.
        vmt("/VMT/Subscribe/Device", "HMD")
        vmt("/VMT/Get/Devices/List")
        seen = {}
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < 3.0 and not {"/VMT/Out/Alive", "/VMT/Out/SubscribedDevice",
                                                       "/VMT/Out/Devices/List"} <= set(seen):
            hmd()
            try:
                data, _ = out.recvfrom(65536)
            except socket.timeout:
                continue
            address, a = osc_decode(data)
            seen[address] = a
        vmt("/VMT/Unsubscribe/Device", "HMD")
        sub = seen.get("/VMT/Out/SubscribedDevice")
        info["vmt_out_alive"] = seen.get("/VMT/Out/Alive")
        check("VMT /VMT/Out/Alive is sent to port 39571", "/VMT/Out/Alive" in seen, str(seen.get("/VMT/Out/Alive")))
        check("VMT /VMT/Out/SubscribedDevice reports the HMD pose",
              sub is not None and sub[0] == "HMD" and dist(sub[1:4], hmd_pos) < TOL,
              str([round(v, 3) if isinstance(v, float) else v for v in sub]) if sub else "not received")
        devices = seen.get("/VMT/Out/Devices/List")
        check("VMT /VMT/Out/Devices/List lists the devices", devices is not None and "VHMD-HMD-0" in devices[0],
              (devices[0].replace("\n", " ")[:120] if devices else "not received"))

        # 5. Reset: every device VMT drove becomes invalid.
        vmt("/VMT/Reset")
        t0 = time.perf_counter()
        invalid = False
        while time.perf_counter() - t0 < 3.0 and not invalid:
            states = [pose(s) for s in ("VHMD-TRK-5", "VHMD-TRK-6", "VHMD-TRK-7")]
            invalid = all(s is not None and not s[0] for s in states)
            time.sleep(0.02)
        check("VMT /VMT/Reset invalidates the devices VMT drove", invalid)
        client.pose("hmd", (0.0, 1.6, 0.0))
    finally:
        send.close()
        out.close()
        client.close()
        openvr.shutdown()
    print(json.dumps({"results": results, "info": info}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
