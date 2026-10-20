"""Eye gaze check through SteamVR Input, run as its own process by run_integration.py.

The driver feeds `gaze` messages into the HMD's "/eyetracking" component. This
reads them back the way an OpenVR app does: an "eyetracking" action (bound to
/user/head/eyetracking in tests/integration/actions/) and
IVRInput::GetEyeTrackingDataRelativeToNow / GetEyeTrackingDataForNextFrame.
pyopenvr does not wrap those (added in IVRInput_011), so they are called
through the C function table.

Also pins down the coordinate space: the driver sends origin/target relative
to the HMD, and apps must get them in tracking space with the HMD pose applied.
Prints one JSON line: {"results": [...], "info": {...}}.

    python tests/integration/eye_check.py [--port 39570]
"""

from __future__ import annotations

import argparse
import ctypes
import json
import subprocess
import sys
import time
from ctypes import CFUNCTYPE, POINTER, Structure, byref, c_float, c_int, c_uint32, c_uint64, c_void_p, sizeof
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "clients" / "python"))
sys.path.insert(0, str(HERE))

import openvr  # noqa: E402

import steamvr_util  # noqa: E402
from vhmd_client import VHMDClient, math3d  # noqa: E402

VRCMD = steamvr_util.vrcmd_path()
TOL_M = 0.002

# HMD pose, gaze origin and target relative to the HMD. Apps must see
# origin/target transformed by the HMD pose.
CASES = [
    ("identity, straight ahead", (0.0, 1.6, 0.0), (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.0, 0.0, -1.0)),
    ("identity, right and up", (0.0, 1.6, 0.0), (1.0, 0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (0.5, 0.2, -1.0)),
    ("moved, yaw 90, eye offset", (1.0, 1.6, 2.0), math3d.from_euler(yaw=90), (0.03, 0.0, 0.0), (0.5, 0.2, -1.0)),
]


class IVRInput011(Structure):
    """Leading part of VR_IVRInput_FnTable (openvr_capi.h, IVRInput_011)."""
    _fields_ = [(name, c_void_p) for name in (
        "SetActionManifestPath", "GetActionSetHandle", "GetActionHandle", "GetInputSourceHandle",
        "UpdateActionState", "GetDigitalActionData", "GetAnalogActionData", "GetPoseActionDataRelativeToNow",
        "GetPoseActionDataForNextFrame", "GetSkeletalActionData", "GetDominantHand", "SetDominantHand")] + [
        ("GetEyeTrackingDataRelativeToNow",
         CFUNCTYPE(c_int, c_uint64, c_int, c_float, POINTER(openvr.VREyeTrackingData_t), c_uint32)),
        ("GetEyeTrackingDataForNextFrame",
         CFUNCTYPE(c_int, c_uint64, c_int, POINTER(openvr.VREyeTrackingData_t), c_uint32)),
    ]


def eye_functions() -> IVRInput011:
    err = openvr.EVRInitError()
    p = openvr._openvr.VR_GetGenericInterface(b"FnTable:IVRInput_011", byref(err))
    if not p:
        raise RuntimeError(f"FnTable:IVRInput_011 not available (error {err.value})")
    return ctypes.cast(p, POINTER(IVRInput011)).contents


def expected(pos, rot, origin, target):
    o = math3d.rotate(rot, origin)
    t = math3d.rotate(rot, target)
    return tuple(p + v for p, v in zip(pos, o)), tuple(p + v for p, v in zip(pos, t))


def dist(a, b) -> float:
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=39570)
    args = ap.parse_args()

    results, info = [], {}
    system = openvr.init(openvr.VRApplication_Scene)
    compositor = openvr.VRCompositor()
    overlay = openvr.VROverlay()
    vrinput = openvr.VRInput()
    poses = (openvr.TrackedDevicePose_t * openvr.k_unMaxTrackedDeviceCount)()
    client = VHMDClient(port=args.port, timeout=2.0)
    try:
        fns = eye_functions()
        vrinput.setActionManifestPath(str(HERE / "actions" / "actions.json"))
        aset = vrinput.getActionSetHandle("/actions/test")
        gaze = vrinput.getActionHandle("/actions/test/in/gaze")
        active = (openvr.VRActiveActionSet_t * 1)()
        active[0].ulActionSet = aset

        def pump():
            compositor.waitGetPoses(poses, None)
            vrinput.updateActionState(active)

        def read(next_frame=False):
            d = openvr.VREyeTrackingData_t()
            if next_frame:
                e = fns.GetEyeTrackingDataForNextFrame(gaze, openvr.TrackingUniverseStanding, byref(d), sizeof(d))
            else:
                e = fns.GetEyeTrackingDataRelativeToNow(gaze, openvr.TrackingUniverseStanding, 0.0, byref(d), sizeof(d))
            return e, d

        def wait_for(pred, resend, timeout=3.0):
            t0, last = time.perf_counter(), 0.0
            while time.perf_counter() - t0 < timeout:
                if time.perf_counter() - last > 0.05:
                    resend()
                    last = time.perf_counter()
                pump()
                if pred():
                    return True
            return False

        # Dashboard holds focus right after SteamVR starts.
        t0 = time.perf_counter()
        ready = False
        while time.perf_counter() - t0 < 30 and not ready:
            if overlay.isDashboardVisible():
                subprocess.run([str(VRCMD), "--hidedashboard"], capture_output=True, timeout=10)
            client.gaze(target=(0, 0, -1))
            pump()
            e, d = read()
            ready = e == 0 and bool(d.bActive)
        info["gaze_action_ready_s"] = round(time.perf_counter() - t0, 2)
        results.append({"name": "gaze: eyetracking action bound to /user/head/eyetracking is active",
                        "ok": ready, "detail": f"error {read()[0]}"})

        for label, pos, rot, origin, target in CASES:
            want_o, want_t = expected(pos, rot, origin, target)

            def resend():
                client.pose("hmd", pos, rot)
                client.gaze(origin=origin, target=target)

            def matches(next_frame=False):
                e, d = read(next_frame)
                return (e == 0 and d.bValid and d.bTracked and dist(d.vGazeOrigin.v, want_o) < TOL_M
                        and dist(d.vGazeTarget.v, want_t) < TOL_M)

            ok = wait_for(matches, resend)
            e, d = read()
            got_o, got_t = tuple(d.vGazeOrigin.v), tuple(d.vGazeTarget.v)
            results.append({"name": f"gaze reaches apps in tracking space, HMD pose applied ({label})", "ok": ok,
                            "detail": f"target {tuple(round(v, 3) for v in got_t)} want {tuple(round(v, 3) for v in want_t)}, "
                                      f"origin {tuple(round(v, 3) for v in got_o)} want {tuple(round(v, 3) for v in want_o)}"})
        results.append({"name": "gaze: GetEyeTrackingDataForNextFrame returns the same data",
                        "ok": wait_for(lambda: matches(next_frame=True), resend), "detail": ""})

        invalid = wait_for(lambda: read()[0] == 0 and not read()[1].bValid and not read()[1].bTracked,
                           lambda: client.gaze(valid=False))
        results.append({"name": "gaze valid=false reaches apps as bValid/bTracked false", "ok": invalid, "detail": ""})
    finally:
        client.close()
        openvr.shutdown()
    print(json.dumps({"results": results, "info": info}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
