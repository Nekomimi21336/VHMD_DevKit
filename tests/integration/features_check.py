"""Device feature check, run as its own process by run_integration.py.

Covers what apps see of: the HMD proximity sensor, battery status, the extra
controller inputs (trackpad, grip force, thumbrest), tracker inputs, pose
sample age (poseTimeOffset), the play area and localized input names. Runs as
a Scene app pumping WaitGetPoses (input needs focus). Prints one JSON line:
{"results": [...], "info": {...}}.

    python tests/integration/features_check.py [--port 39570]
"""

from __future__ import annotations

import argparse
import ctypes
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "clients" / "python"))
sys.path.insert(0, str(HERE))

import openvr  # noqa: E402

import steamvr_util  # noqa: E402
from vhmd_client import VHMDClient  # noqa: E402

VRCMD = steamvr_util.vrcmd_path()
LOCALIZED = ("VirtualHMD", "Trigger", "トリガー", "扳机", "트리거")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=39570)
    args = ap.parse_args()

    results, info = [], {}

    def check(name, ok, detail=""):
        results.append({"name": name, "ok": bool(ok), "detail": detail})

    system = openvr.init(openvr.VRApplication_Scene)
    compositor = openvr.VRCompositor()
    overlay = openvr.VROverlay()
    vrinput = openvr.VRInput()
    poses = (openvr.TrackedDevicePose_t * openvr.k_unMaxTrackedDeviceCount)()
    client = VHMDClient(port=args.port, timeout=2.0)
    try:
        vrinput.setActionManifestPath(str(HERE / "actions" / "actions.json"))
        aset = vrinput.getActionSetHandle("/actions/test")
        act = {n: vrinput.getActionHandle(f"/actions/test/in/{n}") for n in (
            "trigger", "trackpad", "trackpad_click", "grip_force", "thumbrest", "proximity",
            "tracker_trigger", "tracker_trackpad", "tracker_grip")}
        active = (openvr.VRActiveActionSet_t * 1)()
        active[0].ulActionSet = aset
        none = openvr.k_ulInvalidInputValueHandle

        def pump():
            compositor.waitGetPoses(poses, None)
            vrinput.updateActionState(active)

        def analog(name):
            try:
                return vrinput.getAnalogActionData(act[name], none)
            except openvr.OpenVRError:
                return None

        def digital(name):
            try:
                return vrinput.getDigitalActionData(act[name], none)
            except openvr.OpenVRError:
                return None

        def wait_for(pred, resend=None, timeout=3.0):
            t0, last = time.perf_counter(), 0.0
            while time.perf_counter() - t0 < timeout:
                if resend and time.perf_counter() - last > 0.05:
                    resend()
                    last = time.perf_counter()
                pump()
                try:
                    if pred():
                        return True
                except openvr.OpenVRError:
                    pass
            return False

        def index_of(serial):
            for i in range(openvr.k_unMaxTrackedDeviceCount):
                try:
                    if system.getStringTrackedDeviceProperty(i, openvr.Prop_SerialNumber_String) == serial:
                        return i
                except openvr.OpenVRError:
                    pass
            return None

        # Trackers are added on demand: the first pose registers them.
        client.pose("tracker_0", (0.0, 1.0, 0.0))
        client.pose("tracker_1", (0.3, 1.0, 0.0))

        # Focus: the dashboard holds it right after SteamVR starts.
        t0 = time.perf_counter()
        ready = False
        while time.perf_counter() - t0 < 30 and not ready:
            if overlay.isDashboardVisible():
                subprocess.run([str(VRCMD), "--hidedashboard"], capture_output=True, timeout=10)
            client.input("right_hand", "trigger", 0.3)
            pump()
            a = analog("trigger")
            ready = a is not None and a.bActive
        client.input("right_hand", "trigger", 0.0)
        info["features_ready_s"] = round(time.perf_counter() - t0, 2)

        # --- HMD proximity sensor ------------------------------------------
        # The legacy side (k_EButton_ProximitySensor) is checked in input_check.py:
        # apps that set an action manifest get no legacy input.
        hmd = 0
        check("HMD reports a proximity sensor (Prop_ContainsProximitySensor_Bool)",
              system.getBoolTrackedDeviceProperty(hmd, openvr.Prop_ContainsProximitySensor_Bool))
        worn = wait_for(lambda: digital("proximity").bState, resend=lambda: client.proximity(True))
        off = wait_for(lambda: not digital("proximity").bState, resend=lambda: client.proximity(False))
        back = wait_for(lambda: digital("proximity").bState, resend=lambda: client.proximity(True))
        check("proximity sensor reaches apps (/user/head/proximity action)", worn and off and back,
              f"worn={worn} removed={off} back={back}")

        # --- battery ----------------------------------------------------------
        right = system.getTrackedDeviceIndexForControllerRole(openvr.TrackedControllerRole_RightHand)

        def battery():
            return (system.getBoolTrackedDeviceProperty(right, openvr.Prop_DeviceProvidesBatteryStatus_Bool),
                    system.getFloatTrackedDeviceProperty(right, openvr.Prop_DeviceBatteryPercentage_Float),
                    system.getBoolTrackedDeviceProperty(right, openvr.Prop_DeviceIsCharging_Bool))

        initial = battery()
        changed = wait_for(lambda: abs(battery()[1] - 0.42) < 1e-4 and battery()[2],
                           resend=lambda: client.battery("right_hand", 0.42, charging=True))
        got = battery()
        client.battery("right_hand", 1.0, charging=False)
        hmd_battery = system.getBoolTrackedDeviceProperty(hmd, openvr.Prop_DeviceProvidesBatteryStatus_Bool)
        check("battery: controllers report a full battery until told otherwise; HMD has none",
              initial[0] and abs(initial[1] - 1.0) < 1e-4 and not initial[2] and not hmd_battery,
              f"controller {initial}, HMD provides={hmd_battery}")
        check("battery level and charging reach apps", changed, f"{got}")

        # --- extra controller inputs -------------------------------------
        tp = wait_for(lambda: abs(analog("trackpad").x - 0.5) < 1e-3 and abs(analog("trackpad").y + 0.25) < 1e-3,
                      resend=lambda: client.inputs("right_hand", {"trackpad_x": 0.5, "trackpad_y": -0.25,
                                                                  "trackpad_touch": True}))
        tpc = wait_for(lambda: digital("trackpad_click").bState,
                       resend=lambda: client.input("right_hand", "trackpad_click", True))
        info["trackpad_click_state"] = str(digital("trackpad_click").bState if digital("trackpad_click") else None)
        client.inputs("right_hand", {"trackpad_x": 0, "trackpad_y": 0, "trackpad_touch": False, "trackpad_click": False})
        check("trackpad position and click reach apps", tp and tpc, f"position={tp} click={tpc}")
        gf = wait_for(lambda: abs(analog("grip_force").x - 0.7) < 1e-3,
                      resend=lambda: client.input("right_hand", "grip_force", 0.7))
        client.input("right_hand", "grip_force", 0.0)
        check("grip force reaches apps", gf, f"{analog('grip_force').x if analog('grip_force') else None}")
        tr = wait_for(lambda: digital("thumbrest").bState, resend=lambda: client.input("right_hand", "thumbrest_touch", True))
        client.input("right_hand", "thumbrest_touch", False)
        check("thumbrest touch reaches apps", tr)

        # --- tracker inputs (tracker_0 = waist) --------------------------------
        tt = wait_for(lambda: abs(analog("tracker_trigger").x - 0.8) < 1e-3,
                      resend=lambda: client.input("tracker_0", "trigger", 0.8), timeout=5.0)
        ttp = wait_for(lambda: abs(analog("tracker_trackpad").x + 0.4) < 1e-3,
                       resend=lambda: client.inputs("tracker_0", {"trackpad_x": -0.4, "trackpad_y": 0.6}))
        tg = wait_for(lambda: digital("tracker_grip").bState, resend=lambda: client.input("tracker_0", "grip", True))
        client.inputs("tracker_0", {"trigger": 0.0, "trackpad_x": 0.0, "trackpad_y": 0.0, "grip": False})
        check("tracker inputs reach apps (trigger, trackpad, grip on /user/waist)", tt and ttp and tg,
              f"trigger={tt} trackpad={ttp} grip={tg}")

        # --- pose sample age -> poseTimeOffset ---------------------------------
        trk1 = index_of("VHMD-TRK-1")
        vel = 1.0  # m/s along +x

        def mean_x(age):
            xs = []
            t1, last = time.perf_counter(), 0.0
            while time.perf_counter() - t1 < 1.5:
                if time.perf_counter() - last > 0.01:
                    client.pose("tracker_1", (0.0, 1.0, 0.0), vel=(vel, 0.0, 0.0), age=age)
                    last = time.perf_counter()
                pump()
                if time.perf_counter() - t1 > 0.5:
                    p = system.getDeviceToAbsoluteTrackingPose(openvr.TrackingUniverseStanding, 0.0, trk1 + 1)[trk1]
                    xs.append(p.mDeviceToAbsoluteTracking.m[0][3])
            return sum(xs) / len(xs) if xs else float("nan")

        if trk1 is None:
            check("pose age moves the prediction back (poseTimeOffset)", False, "tracker_1 not registered")
        else:
            base, aged = mean_x(None), mean_x(0.05)
            client.pose("tracker_1", (0.3, 1.0, 0.0))
            shift = aged - base
            check("pose age moves the prediction back (poseTimeOffset): age 50 ms at 1 m/s -> +5 cm",
                  abs(shift - 0.05) < 0.015, f"mean x {base:.4f} -> {aged:.4f} (shift {shift * 100:.1f} cm)")

        # --- play area and localized names --------------------------------------
        ok, w, d = openvr.VRChaperone().getPlayAreaSize()
        check("play area from the driver chaperone (default 2 x 2 m)", ok and abs(w - 2) < 1e-3 and abs(d - 2) < 1e-3,
              f"{w:.2f} x {d:.2f}")
        names = []
        try:
            # pyopenvr's getActionOrigins passes byref() an int; call the table directly.
            origins = (openvr.VRInputValueHandle_t * openvr.k_unMaxActionOriginCount)()
            vrinput.function_table.getActionOrigins(aset, act["trigger"], origins, len(origins))
            for origin in origins:
                if origin != none:
                    # pyopenvr asks for the size with a null buffer first, which SteamVR
                    # does not support (access violation); use a fixed buffer.
                    buf = ctypes.create_string_buffer(512)
                    vrinput.function_table.getOriginLocalizedName(origin, buf, len(buf), openvr.VRInputString_All)
                    try:
                        names.append(buf.value.decode("utf-8"))
                    except UnicodeDecodeError:  # SteamVR answered in the ANSI code page (seen on Japanese Windows)
                        names.append(buf.value.decode("mbcs" if sys.platform == "win32" else "latin-1", "replace"))
        except openvr.OpenVRError as e:
            names.append(f"error {type(e).__name__}")
        info["trigger_origin_names"] = names
        check("localized input names from resources/localization (trigger origin)",
              any(any(k in n for k in LOCALIZED) for n in names), "; ".join(names))
    finally:
        client.close()
        openvr.shutdown()
    print(json.dumps({"results": results, "info": info}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
