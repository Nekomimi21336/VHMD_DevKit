"""Controller input + haptics check, run as its own process by run_integration.py.

Connects as a Scene app that pumps WaitGetPoses() (only the focused scene app
sees controller input) and
reads the legacy controller state, which SteamVR derives from
driver/resources/input/legacy_bindings_vhmd_controller.json. Prints one JSON
line: {"results": [{"name", "ok", "detail"}], "info": {...}}.

    python tests/integration/input_check.py [--port 39570]
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "clients" / "python"))

import openvr  # noqa: E402

sys.path.insert(0, str(HERE))
import steamvr_util  # noqa: E402

VRCMD = steamvr_util.vrcmd_path()

from vhmd_client import VHMDClient  # noqa: E402


def B(bit: int) -> int:
    return 1 << bit


CASES = [
    # (device, values, predicate on VRControllerState_t, description)
    ("right_hand", {"trigger": 1.0},
     lambda s: abs(s.rAxis[1].x - 1.0) < 1e-3 and s.ulButtonPressed & B(openvr.k_EButton_SteamVR_Trigger),
     "trigger -> axis1=1.0 + trigger button"),
    ("right_hand", {"trigger": 0.4},
     lambda s: abs(s.rAxis[1].x - 0.4) < 1e-3 and not s.ulButtonPressed & B(openvr.k_EButton_SteamVR_Trigger),
     "trigger 0.4 -> axis1=0.4, no click (auto threshold 0.75)"),
    ("right_hand", {"trigger": 0.0, "grip": 1.0},
     lambda s: s.ulButtonPressed & B(openvr.k_EButton_Grip) and abs(s.rAxis[2].x - 1.0) < 1e-3,
     "grip -> grip button + axis2"),
    ("right_hand", {"grip": 0.0, "joystick_x": 0.5, "joystick_y": -0.25},
     lambda s: abs(s.rAxis[0].x - 0.5) < 1e-3 and abs(s.rAxis[0].y + 0.25) < 1e-3,
     "joystick -> axis0 (0.5, -0.25)"),
    ("right_hand", {"joystick_x": 0, "joystick_y": 0, "a": True},
     lambda s: s.ulButtonPressed & B(openvr.k_EButton_A), "A -> A button"),
    ("right_hand", {"a": False, "secondary": True},
     lambda s: s.ulButtonPressed & B(openvr.k_EButton_ApplicationMenu), "secondary(B) -> application menu"),
    ("left_hand", {"primary": True},
     lambda s: s.ulButtonPressed & B(openvr.k_EButton_A), "left primary(X) -> A button"),
    ("left_hand", {"/input/y/click": 1},
     lambda s: s.ulButtonPressed & B(openvr.k_EButton_ApplicationMenu), "raw path /input/y/click"),
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=39570)
    args = ap.parse_args()

    results, info = [], {}
    system = openvr.init(openvr.VRApplication_Scene)
    compositor = openvr.VRCompositor()
    poses = (openvr.TrackedDevicePose_t * openvr.k_unMaxTrackedDeviceCount)()

    def pump() -> None:
        # A Scene app that never calls WaitGetPoses() is not treated as the
        # active app and gets no input focus (getControllerState() fails).
        compositor.waitGetPoses(poses, None)

    client = VHMDClient(port=args.port, timeout=2.0)
    try:
        idx = {d["id"]: d["index"] for d in client.status()["devices"] if d["index"] >= 0}

        # Right after startup SteamVR shows its dashboard, which holds input
        # focus; close it with SteamVR's own vrcmd tool.
        overlay = openvr.VROverlay()
        t0 = time.perf_counter()
        hidden = 0
        while time.perf_counter() - t0 < 30.0 and not system.getControllerState(idx["right_hand"])[0]:
            if overlay.isDashboardVisible() and hidden < 5:
                subprocess.run([str(VRCMD), "--hidedashboard"], capture_output=True, timeout=10)
                hidden += 1
            pump()
        info["dashboard_hide_calls"] = hidden
        info["scene_app_input_ready_s"] = round(time.perf_counter() - t0, 2)

        passed, failed = 0, []
        for dev, values, pred, desc in CASES:
            ok, last_send = False, 0.0
            t0 = time.perf_counter()
            while time.perf_counter() - t0 < 2.0:
                if time.perf_counter() - last_send > 0.1:
                    client.inputs(dev, values)
                    last_send = time.perf_counter()
                got, st = system.getControllerState(idx[dev])
                if got and pred(st):
                    ok = True
                    break
                pump()
            if ok:
                passed += 1
            else:
                got, st = system.getControllerState(idx[dev])
                failed.append(f"{desc} [got ok={got} pressed={st.ulButtonPressed:#x} "
                              f"axes={[(round(a.x, 3), round(a.y, 3)) for a in st.rAxis[:3]]}]")
        for dev in ("left_hand", "right_hand"):
            client.inputs(dev, {"trigger": 0, "grip": 0, "joystick_x": 0, "joystick_y": 0, "a": 0, "b": 0, "x": 0, "y": 0})
        results.append({"name": f"controller inputs visible to apps ({passed}/{len(CASES)})", "ok": not failed,
                        "detail": "failed: " + "; ".join(failed) if failed else ""})

        # Haptics: a legacy pulse from the app must come back to the UDP client.
        client.poll_haptics()
        client.ping()  # makes this socket the driver's most recent client
        for _ in range(5):
            system.triggerHapticPulse(idx["right_hand"], 0, 3000)
            for _ in range(4):
                pump()
        events = client.poll_haptics(timeout=0.5)
        results.append({"name": "haptic pulse forwarded to UDP client",
                        "ok": any(e.get("id") == "right_hand" for e in events),
                        "detail": f"{len(events)} haptic events, first: {events[0] if events else None}"})

        # HMD proximity sensor through the legacy API (k_EButton_ProximitySensor,
        # mapped by legacy_bindings_vhmd_hmd.json). Apps that set an action
        # manifest get no legacy input, so this lives here.
        bit = 1 << openvr.k_EButton_ProximitySensor

        def proximity_is(worn):
            t1, last = time.perf_counter(), 0.0
            while time.perf_counter() - t1 < 2.0:
                if time.perf_counter() - last > 0.1:
                    client.proximity(worn)
                    last = time.perf_counter()
                got, st = system.getControllerState(idx["hmd"])
                if got and bool(st.ulButtonPressed & bit) == worn:
                    return True
                pump()
            return False

        removed, worn = proximity_is(False), proximity_is(True)
        results.append({"name": "HMD proximity sensor visible to legacy apps (k_EButton_ProximitySensor)",
                        "ok": removed and worn, "detail": f"removed={removed} worn={worn}"})
    finally:
        client.close()
        openvr.shutdown()
    print(json.dumps({"results": results, "info": info}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
