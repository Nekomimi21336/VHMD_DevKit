"""Automated SteamVR integration test for the VirtualHMD driver (requirement 10.1).

Prerequisites: driver built and registered (tools/install_driver.ps1),
pyopenvr installed (pip install openvr).

    python tests/integration/run_integration.py [--keep-running] [--no-start]

Starts SteamVR if needed, then checks:
  1. vrserver.txt shows the driver loading
  2. HMD, 2 controllers and N trackers are enumerated (class / role / serial)
  3. poses sent over UDP show up in GetDeviceToAbsoluteTrackingPose
     (tolerance: 1 mm position, 0.5 deg rotation)
  4. explicit invalidation, timeout policy, garbage packets, 1000 pps load
  5. receive -> pose latency
Writes a JSON report to tests/integration/results/.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import random
import socket
import statistics
import struct
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "clients" / "python"))
sys.path.insert(0, str(HERE))

import openvr  # noqa: E402

import steamvr_util as svr  # noqa: E402
from vhmd_client import VHMDClient, math3d  # noqa: E402

POS_TOL_M = 0.001
ROT_TOL_DEG = 0.5
UNIVERSE = openvr.TrackingUniverseRawAndUncalibrated


class Report:
    def __init__(self):
        self.results = []
        self.info = {}

    def check(self, name: str, ok: bool, detail="") -> bool:
        self.results.append({"name": name, "ok": bool(ok), "detail": detail})
        print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f" -- {detail}" if detail else ""))
        return ok

    @property
    def passed(self) -> bool:
        return all(r["ok"] for r in self.results)


def prop_str(system, idx, prop):
    try:
        return system.getStringTrackedDeviceProperty(idx, prop)
    except openvr.OpenVRError:
        return ""


def find_devices(system):
    """Returns {serial: info} for vhmd devices."""
    devs = {}
    for i in range(openvr.k_unMaxTrackedDeviceCount):
        cls = system.getTrackedDeviceClass(i)
        if cls == openvr.TrackedDeviceClass_Invalid:
            continue
        info = {
            "index": i,
            "class": int(cls),
            "serial": prop_str(system, i, openvr.Prop_SerialNumber_String),
            "tracking_system": prop_str(system, i, openvr.Prop_TrackingSystemName_String),
            "controller_type": prop_str(system, i, openvr.Prop_ControllerType_String),
            "model": prop_str(system, i, openvr.Prop_ModelNumber_String),
            "role": int(system.getControllerRoleForTrackedDeviceIndex(i)),
            "connected": bool(system.isTrackedDeviceConnected(i)),
        }
        devs[info["serial"] or f"#{i}"] = info
    return devs


def get_pose(system, idx):
    poses = system.getDeviceToAbsoluteTrackingPose(UNIVERSE, 0, openvr.k_unMaxTrackedDeviceCount)
    p = poses[idx]
    m = p.mDeviceToAbsoluteTracking
    pos, rot = math3d.from_matrix34([[m[r][c] for c in range(4)] for r in range(3)])
    return p, pos, rot


def wait_pose(system, idx, pos, rot, timeout=1.0):
    """Polls until the device reports pos/rot within tolerance. Returns (ok, dpos, drot, elapsed)."""
    t0 = time.perf_counter()
    dpos = drot = float("inf")
    while time.perf_counter() - t0 < timeout:
        p, gp, gr = get_pose(system, idx)
        if p.bPoseIsValid:
            dpos = math.dist(gp, pos)
            drot = math.degrees(math3d.angle_between(gr, rot))
            if dpos <= POS_TOL_M and drot <= ROT_TOL_DEG:
                return True, dpos, drot, time.perf_counter() - t0
        time.sleep(0.0005)
    return False, dpos, drot, time.perf_counter() - t0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-start", action="store_true", help="do not launch SteamVR")
    ap.add_argument("--keep-running", action="store_true", help="leave SteamVR running afterwards")
    ap.add_argument("--port", type=int, default=39570)
    ap.add_argument("--trackers", type=int, default=8)
    ap.add_argument("--stop-relaunchers", action="store_true",
                    help="terminate an OpenVR overlay app that relaunches SteamVR after shutdown")
    args = ap.parse_args()

    rep = Report()
    started_here = False
    if not svr.running(("vrserver.exe",)):
        if args.no_start:
            print("SteamVR not running and --no-start given")
            return 2
        print("starting SteamVR ...")
        svr.start_steamvr()
        started_here = True
    t_start = time.monotonic()
    # Wait for the driver itself (UDP ping) instead of connecting to OpenVR:
    # the input phase below must be the first OpenVR client from this script.
    probe = VHMDClient(port=args.port, timeout=0.5)
    while not probe.ping():
        if time.monotonic() - t_start > 120:
            print("driver did not answer on UDP within 120 s")
            return 2
    probe.close()
    rep.info["driver_ping_s"] = round(time.monotonic() - t_start, 1)
    time.sleep(3.0)  # let devices finish activating

    # --- controller inputs & haptics (separate process) ------------------------
    # Only the focused Scene app sees controller input. SteamVR identifies both
    # this script and the subprocess as "system.generated.python.exe"; while a
    # process holding that app key is connected (even as a Background app),
    # the second one never gets input focus. So this runs first, in a fresh
    # process, before this script connects (see input_check.py).
    # eye_check.py / openxr_eye_check.py read the gaze back through SteamVR Input
    # and through SteamVR's OpenXR runtime (XR_EXT_eye_gaze_interaction).
    input_phase = {"results": [], "info": {}}
    for script in ("input_check.py", "skeleton_check.py", "eye_check.py", "openxr_eye_check.py"):
        proc = subprocess.run([sys.executable, str(HERE / script), "--port", str(args.port)],
                              capture_output=True, text=True, timeout=180)
        try:
            sub = json.loads(proc.stdout.strip().splitlines()[-1])
        except (ValueError, IndexError):
            sub = {"results": [{"name": script, "ok": False, "detail": (proc.stderr or proc.stdout)[-500:]}]}
        input_phase["results"] += sub["results"]
        input_phase["info"].update(sub.get("info", {}))

    system = openvr.init(openvr.VRApplication_Background)
    rep.info["steamvr_connect_s"] = round(time.monotonic() - t_start, 1)

    client = VHMDClient(port=args.port, timeout=2.0)
    try:
        # --- 1. driver load in vrserver.txt ---------------------------------
        log = svr.latest_session(svr.read_log_since(svr.vrserver_log(), 0))
        loaded = svr.log_lines(log, r"Loaded server driver vhmd")
        listening = svr.log_lines(log, r"vhmd: .*listening on udp")
        rep.check("vrserver.txt: driver loaded", bool(loaded), loaded[0].strip() if loaded else "no 'Loaded server driver vhmd' line")
        rep.check("vrserver.txt: UDP receiver listening", bool(listening), listening[0].strip() if listening else "")
        active_hmd = svr.log_lines(log, r"Active HMD set to")
        rep.info["active_hmd_log"] = [l.strip() for l in active_hmd]
        rep.info["vhmd_warnings"] = [l.strip() for l in svr.log_lines(log, r"vhmd: \[(warn|error)\]")][:20]
        # Errors raised by/for the driver itself. SteamVR's "[Input] <app> (<type>)
        # has no configured binding" notices concern apps, not the driver.
        own_errors = (svr.log_lines(log, r"vhmd: \[error\]") + svr.log_lines(log, r"\[Error\].*driver vhmd")
                      + svr.log_lines(log, r"\[Remapping\].*failed"))
        rep.check("no vhmd/remapping errors in vrserver.txt", not own_errors, "; ".join(l.strip()[:160] for l in own_errors[:3]))

        # --- 2. enumeration ---------------------------------------------------
        status = client.status()
        rep.check("config list reply", status.get("type") == "status", f"{len(status.get('devices', []))} devices in status")
        devs = find_devices(system)
        hands_mode = status.get("hand_mode") == "hands"

        def standby_twin(d):
            # Each hand has a controller and a hand-tracking twin; only the one
            # matching the current hand_mode is connected.
            return ("-HAND-" in d["serial"]) != hands_mode and d["class"] == openvr.TrackedDeviceClass_Controller

        vh = {s: d for s, d in devs.items() if d["tracking_system"] == "vhmd" and not standby_twin(d)}
        rep.info["devices"] = list(devs.values())
        hmd = [d for d in vh.values() if d["class"] == openvr.TrackedDeviceClass_HMD]
        ctl = [d for d in vh.values() if d["class"] == openvr.TrackedDeviceClass_Controller]
        trk = [d for d in vh.values() if d["class"] == openvr.TrackedDeviceClass_GenericTracker]
        hmd_expected = any(d["kind"] == "hmd" and d["enabled"] for d in status["devices"])
        if hmd_expected:
            rep.check("HMD enumerated", len(hmd) == 1, f"{[d['serial'] for d in hmd]}, index {[d['index'] for d in hmd]}")
            rep.check("vhmd HMD is the active HMD (index 0)", bool(hmd) and hmd[0]["index"] == 0,
                      "another driver owns index 0" if hmd and hmd[0]["index"] != 0 else "")
        roles = sorted(d["role"] for d in ctl)
        rep.check("2 controllers with left/right roles", roles == [openvr.TrackedControllerRole_LeftHand, openvr.TrackedControllerRole_RightHand],
                  str([(d["serial"], d["role"], d["controller_type"]) for d in ctl]))
        on_demand = [d for d in status["devices"] if d["enabled"] and d.get("on_demand")]
        rep.check("3-point base: only HMD + controllers registered before use",
                  len(trk) == args.trackers - len(on_demand), f"{len(trk)} trackers at startup, {len(on_demand)} on demand")

        # Trackers join when a client first sends to them, already in sync.
        sent = {}
        with client.batch():
            for i, d in enumerate(on_demand):
                sent[d["id"]] = ((-0.4 + 0.1 * i, 0.3 + 0.1 * i, 0.2), math3d.from_euler(yaw=15 * i))
                client.pose(d["id"], *sent[d["id"]])
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < 5.0:
            trk = [d for d in find_devices(system).values()
                   if d["tracking_system"] == "vhmd" and d["class"] == openvr.TrackedDeviceClass_GenericTracker]
            if len(trk) >= args.trackers:
                break
            time.sleep(0.02)
        add_ms = (time.perf_counter() - t0) * 1000
        rep.check(f"{args.trackers} generic trackers after first pose (on-demand add)", len(trk) == args.trackers,
                  f"{len(trk)} after {add_ms:.0f} ms: {sorted(d['serial'] for d in trk)}")
        status = client.status()
        idx_now = {d["id"]: d["index"] for d in status["devices"] if d["index"] >= 0}
        synced = all(d in idx_now and wait_pose(system, idx_now[d], p, r)[0] for d, (p, r) in sent.items())
        rep.check("added trackers start at the pose that triggered them", synced)
        vh = {s: d for s, d in find_devices(system).items() if d["tracking_system"] == "vhmd" and not standby_twin(d)}
        rep.check("all vhmd devices connected", all(d["connected"] for d in vh.values()))
        rep.check("own controller types (no impersonation)",
                  all(d["controller_type"].startswith("vhmd_") for d in vh.values()),
                  str(sorted({d["controller_type"] for d in vh.values()})))

        # All vhmd devices share one tracking universe with a driver-provided
        # chaperone. Without it the seated zero pose is an all-zero matrix and
        # OpenXR's LOCAL space comes out upside down.
        universes = set()
        for d in vh.values():
            try:
                universes.add(system.getUint64TrackedDeviceProperty(d["index"], openvr.Prop_CurrentUniverseId_Uint64))
            except openvr.OpenVRError:
                universes.add(None)
        seated_ok, seated = openvr.VRChaperoneSetup().getLiveSeatedZeroPoseToRawTrackingPose()
        identity = all(abs(seated.m[r][c] - (1.0 if r == c else 0.0)) < 1e-6 for r in range(3) for c in range(4))
        calib = openvr.VRChaperone().getCalibrationState()
        rep.check("tracking universe set and calibrated (seated zero = identity)",
                  len(universes) == 1 and None not in universes and seated_ok and identity
                  and calib == openvr.ChaperoneCalibrationState_OK,
                  f"universe {[hex(u) if u is not None else None for u in universes]}, calibration state {calib}")
        idx_by_id = {d["id"]: d["index"] for d in status["devices"] if d["index"] >= 0}

        # --- 3. pose round trip ----------------------------------------------
        rng = random.Random(1234)
        errs_p, errs_r, lat = [], [], []
        all_ok = True
        for dev_id in sorted(idx_by_id):
            for _ in range(5):
                pos = (rng.uniform(-1.5, 1.5), rng.uniform(0.1, 2.0), rng.uniform(-1.5, 1.5))
                rot = math3d.from_euler(rng.uniform(-180, 180), rng.uniform(-60, 60), rng.uniform(-90, 90))
                t_send = time.perf_counter()
                client.pose(dev_id, pos, rot, vel=(0, 0, 0), angvel=(0, 0, 0))
                ok, dp, dr, _ = wait_pose(system, idx_by_id[dev_id], pos, rot)
                lat.append((time.perf_counter() - t_send) * 1000)
                errs_p.append(dp)
                errs_r.append(dr)
                all_ok &= ok
        rep.check("pose round trip within 1 mm / 0.5 deg (all devices x5)", all_ok,
                  f"max dpos={max(errs_p) * 1000:.3f} mm, max drot={max(errs_r):.3f} deg")
        lat_sorted = sorted(lat)
        rep.info["latency_ms"] = {
            "median": round(statistics.median(lat), 2),
            "p95": round(lat_sorted[int(len(lat_sorted) * 0.95) - 1], 2),
            "max": round(max(lat), 2),
            "samples": len(lat),
            "note": "UDP send -> pose visible to an OpenVR client (includes polling)",
        }
        rep.check("median send->visible latency <= 11 ms", statistics.median(lat) <= 11.0, f"{rep.info['latency_ms']}")

        # Batch: HMD + hands + trackers in one datagram.
        frame = {dev_id: ((0.1 * i, 1.0 + 0.01 * i, -0.2), math3d.from_euler(yaw=10 * i)) for i, dev_id in enumerate(sorted(idx_by_id))}
        with client.batch():
            for dev_id, (p, r) in frame.items():
                client.pose(dev_id, p, r)
        batch_ok = all(wait_pose(system, idx_by_id[d], p, r)[0] for d, (p, r) in frame.items())
        rep.check(f"batched update of {len(frame)} devices in one datagram", batch_ok)

        # --- 4a. explicit invalidation & recovery ----------------------------
        t_id = "tracker_0" if "tracker_0" in idx_by_id else sorted(idx_by_id)[-1]
        client.invalidate(t_id)
        time.sleep(0.1)
        p, _, _ = get_pose(system, idx_by_id[t_id])
        invalid_ok = not p.bPoseIsValid
        client.pose(t_id, (0, 1, 0))
        time.sleep(0.1)
        p2, _, _ = get_pose(system, idx_by_id[t_id])
        rep.check("explicit invalidation then recovery", invalid_ok and p2.bPoseIsValid,
                  f"invalid={not p.bPoseIsValid}, recovered={p2.bPoseIsValid}")

        # --- 4b. timeout policy ------------------------------------------------
        policy = next(d["timeout_policy"] for d in status["devices"] if d["id"] == t_id)
        timeout_s = status.get("timeout_ms", 500) / 1000
        client.pose(t_id, (0.2, 1.2, 0.0))
        time.sleep(timeout_s + 0.4)
        # (the tracker under test gets no traffic during this sleep)
        p, gp, _ = get_pose(system, idx_by_id[t_id])
        connected = system.isTrackedDeviceConnected(idx_by_id[t_id])
        expected = {"hold": (True, True), "invalid": (False, True), "disconnect": (False, False)}[policy]
        rep.check(f"timeout policy '{policy}' after {timeout_s:.1f}s silence", (bool(p.bPoseIsValid), bool(connected)) == expected,
                  f"valid={bool(p.bPoseIsValid)} connected={bool(connected)} pos={tuple(round(v, 3) for v in gp)}")

        # --- 4c. garbage packets ----------------------------------------------
        raw = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        garbage = [
            b"", b"{", b"[]", b"null", b"\x00\xff\xfe" * 100, os.urandom(1400), b"{" * 5000,
            b'{"type":"pose","id":"hmd","pos":[NaN,0,0],"rot":[1,0,0,0]}',
            b'{"type":"pose","id":"hmd","pos":[1e308,1e308,1e308],"rot":[1,0,0,0]}',
            b'{"type":"pose","id":"hmd","pos":[0,1,0],"rot":[0,0,0,0]}',
            b'{"type":"input","id":"right_hand","name":"trigger","value":"x"}',
            b'{"type":"skeleton","id":"left_hand","bones":[1,2,3]}',
            b'{"type":"pose","id":"' + b"A" * 5000 + b'","pos":[0,1,0],"rot":[1,0,0,0]}',
            b"/VMT/Room/Unity\x00,iii\x00\x00\x00\x00\x00",
            b"#bundle\x00" + b"\x00" * 8 + struct.pack(">I", 0xFFFFFFF0),
            json.dumps([{"type": "pose", "id": "hmd"}] * 300).encode(),
        ] + [os.urandom(rng.randint(1, 2000)) for _ in range(200)]
        for g in garbage:
            raw.sendto(g, ("127.0.0.1", args.port))
        raw.sendto(b"\x00" * 65000, ("127.0.0.1", args.port))
        time.sleep(0.3)
        st2 = client.status()
        rep.check(f"{len(garbage) + 1} malformed packets rejected without crash", client.ping() and st2["packets"]["rejected"] > status["packets"]["rejected"],
                  f"rejected counter {status['packets']['rejected']} -> {st2['packets']['rejected']}")

        # --- 4d. 1000 pps load --------------------------------------------------
        ids = sorted(idx_by_id)
        n = 0
        t0 = time.perf_counter()
        next_t = t0
        while time.perf_counter() - t0 < 5.0:
            dev_id = ids[n % len(ids)]
            a = n * 0.01
            client.pose(dev_id, (math.sin(a), 1.2, math.cos(a)), math3d.from_euler(yaw=n % 360))
            n += 1
            next_t += 0.001
            while time.perf_counter() < next_t:
                pass
        elapsed = time.perf_counter() - t0
        time.sleep(0.2)
        st3 = client.status()
        alive = client.ping()
        total_rate = sum(d["rate_hz"] for d in st3["devices"])
        rep.check("1000 pps for 5 s: driver responsive", alive, f"sent {n} packets ({n / elapsed:.0f} pps), driver-side rate sum {total_rate:.0f} Hz")
        final_pos = (0.0, 1.5, -0.5)
        client.pose(ids[0], final_pos)
        rep.check("pose updates still applied after load", wait_pose(system, idx_by_id[ids[0]], final_pos, math3d.IDENTITY)[0])

        # --- update rate seen by an OpenVR client -------------------------------
        hmd_idx = idx_by_id.get("hmd", idx_by_id[ids[0]])
        hmd_id = "hmd" if "hmd" in idx_by_id else ids[0]
        changes, last = 0, None
        t0 = time.perf_counter()
        k = 0
        while time.perf_counter() - t0 < 2.0:
            client.pose(hmd_id, (0.001 * k, 1.6, 0.0))
            k += 1
            _, gp, _ = get_pose(system, hmd_idx)
            if last is not None and gp != last:
                changes += 1
            last = gp
            time.sleep(0.001)
        rep.info["observed_pose_change_rate_hz"] = round(changes / 2.0, 1)
        rep.check("pose update rate >= 90 Hz (observed by client)", changes / 2.0 >= 90, f"{changes / 2.0:.1f} Hz")

        for r in input_phase["results"]:
            rep.check(r["name"], r["ok"], r["detail"])
        rep.info.update(input_phase.get("info", {}))

        # Restore defaults for the next tester
        client.reset()
        rep.info["final_status"] = client.status()
    finally:
        client.close()
        openvr.shutdown()

    if started_here and not args.keep_running:
        try:
            took = svr.stop_steamvr(timeout=60, stop_relaunchers=args.stop_relaunchers)
            log = svr.latest_session(svr.read_log_since(svr.vrserver_log(), 0))
            clean = bool(svr.log_lines(log, r"vhmd: shutting down"))
            rep.check("SteamVR shutdown: driver Cleanup ran, vrserver exited", clean, f"{took:.1f}s")
        except (TimeoutError, RuntimeError) as e:
            rep.check("SteamVR shutdown", False, str(e))

    out_dir = HERE / "results"
    out_dir.mkdir(exist_ok=True)
    out = out_dir / f"integration_{datetime.now():%Y%m%d_%H%M%S}.json"
    out.write_text(json.dumps({"passed": rep.passed, "results": rep.results, "info": rep.info}, indent=2, default=str), encoding="utf-8")
    print(f"\n{'ALL PASSED' if rep.passed else 'FAILURES'}: {sum(r['ok'] for r in rep.results)}/{len(rep.results)}  report: {out}")
    return 0 if rep.passed else 1


if __name__ == "__main__":
    sys.exit(main())
