"""Hand skeleton + SteamVR Input check, run as its own process by run_integration.py.

Uses SteamVR Input (action manifest in tests/integration/actions/) rather than
the legacy API, so it also covers the vhmd_controller input profile path.
Prints one JSON line: {"results": [...], "info": {...}}.

    python tests/integration/skeleton_check.py [--port 39570]
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "clients" / "python"))
sys.path.insert(0, str(HERE))

import openvr  # noqa: E402

import steamvr_util  # noqa: E402
from vhmd_client import VHMDClient, math3d  # noqa: E402

VRCMD = steamvr_util.steamvr_path() / "bin" / "win64" / "vrcmd.exe"
INDEX_PROXIMAL = 7  # eBone_IndexFinger1


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
        vrinput.setActionManifestPath(str(HERE / "actions" / "actions.json"))
        aset = vrinput.getActionSetHandle("/actions/test")
        sk = {s: vrinput.getActionHandle(f"/actions/test/in/skeleton_{s}") for s in ("left", "right")}
        trig = vrinput.getActionHandle("/actions/test/in/trigger")
        active = (openvr.VRActiveActionSet_t * 1)()
        active[0].ulActionSet = aset

        def pump():
            compositor.waitGetPoses(poses, None)
            vrinput.updateActionState(active)

        def bones(side):
            arr = (openvr.VRBoneTransform_t * 31)()
            vrinput.getSkeletalBoneData(sk[side], openvr.VRSkeletalTransformSpace_Parent,
                                        openvr.VRSkeletalMotionRange_WithoutController, arr)
            return arr

        def wait_for(pred, timeout=3.0, resend=None):
            t0, last = time.perf_counter(), 0.0
            while time.perf_counter() - t0 < timeout:
                if resend and time.perf_counter() - last > 0.1:
                    resend()
                    last = time.perf_counter()
                pump()
                try:
                    if pred():
                        return True
                except openvr.OpenVRError:
                    pass
            return False

        def skeleton_active(side):
            try:
                return vrinput.getSkeletalActionData(sk[side]).bActive
            except openvr.OpenVRError:  # before the first updateActionState()
                return False

        # Dashboard holds focus right after SteamVR starts.
        t0 = time.perf_counter()
        pump()
        while time.perf_counter() - t0 < 30 and not skeleton_active("right"):
            if overlay.isDashboardVisible():
                subprocess.run([str(VRCMD), "--hidedashboard"], capture_output=True, timeout=10)
            pump()
        info["skeleton_action_ready_s"] = round(time.perf_counter() - t0, 2)
        results.append({"name": "skeleton actions active for both hands",
                        "ok": all(skeleton_active(s) for s in sk), "detail": ""})

        # Curl-driven skeleton: index curl from the summary data must follow.
        def curl_of(side):
            return vrinput.getSkeletalSummaryData(sk[side], openvr.VRSummaryType_FromDevice).flFingerCurl[1]

        open_ok = wait_for(lambda: curl_of("right") < 0.2,
                           resend=lambda: client.skeleton_curl("right_hand", [0, 0, 0, 0, 0]))
        open_curl = curl_of("right")
        fist_ok = wait_for(lambda: curl_of("right") > 0.6,
                           resend=lambda: client.skeleton_curl("right_hand", [1, 1, 1, 1, 1]))
        results.append({"name": "skeleton curl message changes finger curl seen by apps",
                        "ok": open_ok and fist_ok,
                        "detail": f"index curl open={open_curl:.2f} fist={curl_of('right'):.2f}"})

        # Explicit bones: a rotation sent for the index proximal bone must come back.
        ref = bones("left")
        rots = [(b.orientation.w, b.orientation.x, b.orientation.y, b.orientation.z) for b in ref]
        target = math3d.normalize(math3d.multiply(rots[INDEX_PROXIMAL], math3d.from_euler(roll=40)))
        rots[INDEX_PROXIMAL] = target

        def matches():
            o = bones("left")[INDEX_PROXIMAL].orientation
            return math.degrees(math3d.angle_between((o.w, o.x, o.y, o.z), target)) < 1.0

        bones_ok = wait_for(matches, resend=lambda: client.skeleton_bones("left_hand", rots))
        o = bones("left")[INDEX_PROXIMAL].orientation
        err = math.degrees(math3d.angle_between((o.w, o.x, o.y, o.z), target))
        results.append({"name": "explicit bone rotations reach apps (index proximal, < 1 deg)",
                        "ok": bones_ok, "detail": f"error {err:.3f} deg"})

        # Inputs through SteamVR Input (profile + binding, not legacy).
        trig_ok = wait_for(lambda: abs(vrinput.getAnalogActionData(trig, openvr.k_ulInvalidInputValueHandle).x - 0.6) < 1e-3,
                           resend=lambda: client.input("right_hand", "trigger", 0.6))
        client.input("right_hand", "trigger", 0.0)
        results.append({"name": "trigger value via SteamVR Input action", "ok": trig_ok, "detail": ""})

        # Hand pose derived from buttons when no skeleton is streamed (after 500 ms).
        time.sleep(0.7)
        derived_ok = wait_for(lambda: curl_of("right") > 0.6,
                              resend=lambda: client.inputs("right_hand", {"grip": 1.0, "trigger": 1.0}))
        client.inputs("right_hand", {"grip": 0.0, "trigger": 0.0})
        results.append({"name": "finger curl derived from trigger/grip when no skeleton is streamed",
                        "ok": derived_ok, "detail": f"index curl {curl_of('right'):.2f}"})
    finally:
        client.close()
        openvr.shutdown()
    print(json.dumps({"results": results, "info": info}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
