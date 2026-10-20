"""Eye gaze check through SteamVR's OpenXR runtime, run as its own process by run_integration.py.

Opens a headless OpenXR session (XR_MND_headless, no graphics) on SteamVR's
own OpenXR loader, binds a pose action to XR_EXT_eye_gaze_interaction
(/user/eyes_ext/input/gaze_ext/pose) and locates it in VIEW and STAGE space
while gaze messages are sent to the driver. Also checks that the head pose in
LOCAL space matches STAGE (the vhmd universe is calibrated at identity).
Plain ctypes, so no extra Python package is needed. Prints one JSON line: {"results": [...], "info": {...}}.

    python tests/integration/openxr_eye_check.py [--port 39570]
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import sys
import time
from ctypes import (CFUNCTYPE, POINTER, Structure, byref, c_char, c_char_p, c_float, c_int32, c_int64, c_uint8,
                    c_uint32, c_uint64, c_void_p, cast)
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "clients" / "python"))
sys.path.insert(0, str(HERE))

import steamvr_util  # noqa: E402
from vhmd_client import VHMDClient, math3d  # noqa: E402

from eye_check import CASES, TOL_M  # noqa: E402

WINDOWS = sys.platform == "win32"
H = c_uint64  # handles, XrPath, XrSystemId


def S(name, fields):
    return type(name, (Structure,), {"_fields_": fields})


Quat = S("XrQuaternionf", [("x", c_float), ("y", c_float), ("z", c_float), ("w", c_float)])
Vec3 = S("XrVector3f", [("x", c_float), ("y", c_float), ("z", c_float)])
Pose = S("XrPosef", [("orientation", Quat), ("position", Vec3)])
AppInfo = S("XrApplicationInfo", [("applicationName", c_char * 128), ("applicationVersion", c_uint32),
                                  ("engineName", c_char * 128), ("engineVersion", c_uint32), ("apiVersion", c_uint64)])
InstanceCI = S("XrInstanceCreateInfo", [("type", c_int32), ("next", c_void_p), ("createFlags", c_uint64),
                                        ("applicationInfo", AppInfo), ("enabledApiLayerCount", c_uint32),
                                        ("enabledApiLayerNames", c_void_p), ("enabledExtensionCount", c_uint32),
                                        ("enabledExtensionNames", POINTER(c_char_p))])
ExtProps = S("XrExtensionProperties", [("type", c_int32), ("next", c_void_p), ("extensionName", c_char * 128),
                                       ("extensionVersion", c_uint32)])
SystemGI = S("XrSystemGetInfo", [("type", c_int32), ("next", c_void_p), ("formFactor", c_int32)])
EyeProps = S("XrSystemEyeGazeInteractionPropertiesEXT", [("type", c_int32), ("next", c_void_p),
                                                         ("supportsEyeGazeInteraction", c_uint32)])
SysProps = S("XrSystemProperties", [("type", c_int32), ("next", c_void_p), ("systemId", c_uint64), ("vendorId", c_uint32),
                                    ("systemName", c_char * 256), ("maxHeight", c_uint32), ("maxWidth", c_uint32),
                                    ("maxLayers", c_uint32), ("orientationTracking", c_uint32),
                                    ("positionTracking", c_uint32)])
SessionCI = S("XrSessionCreateInfo", [("type", c_int32), ("next", c_void_p), ("createFlags", c_uint64), ("systemId", H)])
ActionSetCI = S("XrActionSetCreateInfo", [("type", c_int32), ("next", c_void_p), ("actionSetName", c_char * 64),
                                          ("localizedActionSetName", c_char * 128), ("priority", c_uint32)])
ActionCI = S("XrActionCreateInfo", [("type", c_int32), ("next", c_void_p), ("actionName", c_char * 64),
                                    ("actionType", c_int32), ("countSubactionPaths", c_uint32),
                                    ("subactionPaths", c_void_p), ("localizedActionName", c_char * 128)])
SuggBind = S("XrActionSuggestedBinding", [("action", H), ("binding", H)])
ProfileSB = S("XrInteractionProfileSuggestedBinding", [("type", c_int32), ("next", c_void_p), ("interactionProfile", H),
                                                       ("count", c_uint32), ("bindings", POINTER(SuggBind))])
AttachInfo = S("XrSessionActionSetsAttachInfo", [("type", c_int32), ("next", c_void_p), ("count", c_uint32),
                                                 ("sets", POINTER(H))])
ActionSpaceCI = S("XrActionSpaceCreateInfo", [("type", c_int32), ("next", c_void_p), ("action", H),
                                              ("subactionPath", H), ("pose", Pose)])
RefSpaceCI = S("XrReferenceSpaceCreateInfo", [("type", c_int32), ("next", c_void_p), ("referenceSpaceType", c_int32),
                                              ("pose", Pose)])
BeginInfo = S("XrSessionBeginInfo", [("type", c_int32), ("next", c_void_p), ("viewConfigurationType", c_int32)])
EventBuf = S("XrEventDataBuffer", [("type", c_int32), ("next", c_void_p), ("varying", c_uint8 * 4000)])
StateChanged = S("XrEventDataSessionStateChanged", [("type", c_int32), ("next", c_void_p), ("session", H),
                                                    ("state", c_int32), ("time", c_int64)])
ActiveSet = S("XrActiveActionSet", [("actionSet", H), ("subactionPath", H)])
SyncInfo = S("XrActionsSyncInfo", [("type", c_int32), ("next", c_void_p), ("count", c_uint32),
                                   ("sets", POINTER(ActiveSet))])
StateGI = S("XrActionStateGetInfo", [("type", c_int32), ("next", c_void_p), ("action", H), ("subactionPath", H)])
StatePose = S("XrActionStatePose", [("type", c_int32), ("next", c_void_p), ("isActive", c_uint32)])
SpaceLoc = S("XrSpaceLocation", [("type", c_int32), ("next", c_void_p), ("locationFlags", c_uint64), ("pose", Pose)])
Timespec = S("timespec", [("tv_sec", c_int64), ("tv_nsec", c_int64)])

# XrStructureType / enum values from openxr.h
TYPE_EXTENSION_PROPERTIES, TYPE_INSTANCE_CREATE_INFO, TYPE_SYSTEM_GET_INFO, TYPE_SYSTEM_PROPERTIES = 2, 3, 4, 5
TYPE_SESSION_CREATE_INFO, TYPE_SESSION_BEGIN_INFO, TYPE_EVENT_DATA_BUFFER = 8, 10, 16
TYPE_EVENT_DATA_SESSION_STATE_CHANGED, TYPE_ACTION_STATE_POSE = 18, 27
TYPE_ACTION_SET_CREATE_INFO, TYPE_ACTION_CREATE_INFO = 28, 29
TYPE_REFERENCE_SPACE_CREATE_INFO, TYPE_ACTION_SPACE_CREATE_INFO, TYPE_SPACE_LOCATION = 37, 38, 42
TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, TYPE_ACTION_STATE_GET_INFO = 51, 58
TYPE_SESSION_ACTION_SETS_ATTACH_INFO, TYPE_ACTIONS_SYNC_INFO = 60, 61
TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT = 1000030000
FORM_FACTOR_HMD, ACTION_TYPE_POSE, VIEW_CONFIG_STEREO = 1, 4, 2
SPACE_VIEW, SPACE_LOCAL, SPACE_STAGE = 1, 2, 3
STATE_READY, STATE_SYNCHRONIZED, STATE_FOCUSED = 2, 3, 5
LOCATION_VALID = 0x1 | 0x2      # orientation + position valid
LOCATION_TRACKED = 0x4 | 0x8    # orientation + position tracked
IDENTITY = Pose(Quat(0, 0, 0, 1), Vec3(0, 0, 0))
TIME_EXT = (b"XR_KHR_win32_convert_performance_counter_time" if WINDOWS else b"XR_KHR_convert_timespec_time")


class Skip(Exception):
    pass


class XrError(Exception):
    pass


def load_loader():
    svr = steamvr_util.steamvr_path()
    if WINDOWS:
        lib, manifest = svr / "bin" / "win64" / "openxr_loader.dll", svr / "steamxr_win64.json"
    else:
        lib, manifest = svr / "bin" / "linux64" / "libopenxr_loader.so", svr / "steamxr_linux64.json"
    if not lib.exists() or not manifest.exists():
        raise Skip(f"SteamVR OpenXR loader or runtime manifest not found under {svr}")
    os.environ["XR_RUNTIME_JSON"] = str(manifest)  # use SteamVR even if another runtime is active
    return ctypes.CDLL(str(lib))


class EyeGazeSession:
    def __init__(self):
        self.xr = load_loader()
        self.inst, self.sess, self.state = H(), H(), 0

    def ok(self, result, what):
        if result < 0:
            raise XrError(f"{what} failed ({result})")
        return result

    def extensions(self):
        n = c_uint32()
        self.ok(self.xr.xrEnumerateInstanceExtensionProperties(None, 0, byref(n), None), "enumerate extensions")
        props = (ExtProps * n.value)(*[ExtProps(TYPE_EXTENSION_PROPERTIES) for _ in range(n.value)])
        self.ok(self.xr.xrEnumerateInstanceExtensionProperties(None, n, byref(n), props), "enumerate extensions")
        return {p.extensionName for p in props}

    def path(self, s):
        p = H()
        self.ok(self.xr.xrStringToPath(self.inst, s.encode(), byref(p)), "xrStringToPath " + s)
        return p.value

    def open(self):
        xr = self.xr
        wanted = [b"XR_EXT_eye_gaze_interaction", b"XR_MND_headless", TIME_EXT]
        missing = [e.decode() for e in wanted if e not in self.extensions()]
        if missing:
            raise Skip("runtime lacks " + ", ".join(missing))
        names = (c_char_p * len(wanted))(*wanted)
        ci = InstanceCI(TYPE_INSTANCE_CREATE_INFO, None, 0, AppInfo(b"vhmd_openxr_eye_check", 1, b"none", 0, 1 << 48),
                        0, None, len(wanted), names)
        self.ok(xr.xrCreateInstance(byref(ci), byref(self.inst)), "xrCreateInstance")
        system = H()
        self.ok(xr.xrGetSystem(self.inst, byref(SystemGI(TYPE_SYSTEM_GET_INFO, None, FORM_FACTOR_HMD)), byref(system)),
                "xrGetSystem")
        eye = EyeProps(TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT, None, 0)
        props = SysProps(TYPE_SYSTEM_PROPERTIES, ctypes.addressof(eye))
        self.ok(xr.xrGetSystemProperties(self.inst, system, byref(props)), "xrGetSystemProperties")
        self.system_name = props.systemName.decode(errors="replace")
        self.supports_eye_gaze = bool(eye.supportsEyeGazeInteraction)
        # XR_MND_headless: no graphics binding in the create info.
        self.ok(xr.xrCreateSession(self.inst, byref(SessionCI(TYPE_SESSION_CREATE_INFO, None, 0, system.value)),
                                   byref(self.sess)), "xrCreateSession")
        self.aset, self.action = H(), H()
        self.ok(xr.xrCreateActionSet(self.inst, byref(ActionSetCI(TYPE_ACTION_SET_CREATE_INFO, None, b"eye", b"eye", 0)),
                                     byref(self.aset)), "xrCreateActionSet")
        self.ok(xr.xrCreateAction(self.aset, byref(ActionCI(TYPE_ACTION_CREATE_INFO, None, b"gaze", ACTION_TYPE_POSE,
                                                            0, None, b"gaze")), byref(self.action)), "xrCreateAction")
        binding = (SuggBind * 1)(SuggBind(self.action.value, self.path("/user/eyes_ext/input/gaze_ext/pose")))
        profile = self.path("/interaction_profiles/ext/eye_gaze_interaction")
        self.ok(xr.xrSuggestInteractionProfileBindings(
            self.inst, byref(ProfileSB(TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, None, profile, 1, binding))),
            "xrSuggestInteractionProfileBindings")
        sets = (H * 1)(self.aset.value)
        self.ok(xr.xrAttachSessionActionSets(self.sess, byref(AttachInfo(TYPE_SESSION_ACTION_SETS_ATTACH_INFO, None, 1,
                                                                         sets))), "xrAttachSessionActionSets")
        self.gaze_space = H()
        self.ok(xr.xrCreateActionSpace(self.sess, byref(ActionSpaceCI(TYPE_ACTION_SPACE_CREATE_INFO, None,
                                                                      self.action.value, 0, IDENTITY)),
                                       byref(self.gaze_space)), "xrCreateActionSpace")
        self.spaces = {}
        for name, kind in (("view", SPACE_VIEW), ("local", SPACE_LOCAL), ("stage", SPACE_STAGE)):
            s = H()
            self.ok(xr.xrCreateReferenceSpace(self.sess, byref(RefSpaceCI(TYPE_REFERENCE_SPACE_CREATE_INFO, None, kind,
                                                                          IDENTITY)), byref(s)), "xrCreateReferenceSpace")
            self.spaces[name] = s
        fn = c_void_p()
        if WINDOWS:
            self.ok(xr.xrGetInstanceProcAddr(self.inst, b"xrConvertWin32PerformanceCounterToTimeKHR", byref(fn)),
                    "xrGetInstanceProcAddr")
            self._convert = CFUNCTYPE(c_int32, H, POINTER(c_int64), POINTER(c_int64))(fn.value)
        else:
            self.ok(xr.xrGetInstanceProcAddr(self.inst, b"xrConvertTimespecTimeToTimeKHR", byref(fn)),
                    "xrGetInstanceProcAddr")
            self._convert = CFUNCTYPE(c_int32, H, POINTER(Timespec), POINTER(c_int64))(fn.value)
        self.active = (ActiveSet * 1)(ActiveSet(self.aset.value, 0))

    def now(self):
        t = c_int64()
        if WINDOWS:
            pc = c_int64()
            ctypes.windll.kernel32.QueryPerformanceCounter(byref(pc))
            self._convert(self.inst, byref(pc), byref(t))
        else:
            ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            self._convert(self.inst, byref(Timespec(ns // 1_000_000_000, ns % 1_000_000_000)), byref(t))
        return t

    def poll(self):
        while True:
            ev = EventBuf(TYPE_EVENT_DATA_BUFFER, None)
            if self.xr.xrPollEvent(self.inst, byref(ev)) != 0:  # XR_EVENT_UNAVAILABLE
                return
            if ev.type == TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
                self.state = cast(byref(ev), POINTER(StateChanged)).contents.state
                if self.state == STATE_READY:
                    self.ok(self.xr.xrBeginSession(self.sess, byref(BeginInfo(TYPE_SESSION_BEGIN_INFO, None,
                                                                              VIEW_CONFIG_STEREO))), "xrBeginSession")

    def sync(self):
        self.poll()
        if self.state >= STATE_SYNCHRONIZED:
            self.xr.xrSyncActions(self.sess, byref(SyncInfo(TYPE_ACTIONS_SYNC_INFO, None, 1, self.active)))

    def gaze_active(self):
        st = StatePose(TYPE_ACTION_STATE_POSE, None, 0)
        self.xr.xrGetActionStatePose(self.sess, byref(StateGI(TYPE_ACTION_STATE_GET_INFO, None, self.action.value, 0)),
                                     byref(st))
        return bool(st.isActive)

    def locate(self, base, space=None):
        loc = SpaceLoc(TYPE_SPACE_LOCATION, None, 0)
        space = self.gaze_space if space is None else self.spaces[space]
        r = self.xr.xrLocateSpace(space, self.spaces[base], self.now(), byref(loc))
        q, p = loc.pose.orientation, loc.pose.position
        return r, loc.locationFlags, (p.x, p.y, p.z), (q.w, q.x, q.y, q.z)

    def close(self):
        if self.sess.value:
            self.xr.xrDestroySession(self.sess)
        if self.inst.value:
            self.xr.xrDestroyInstance(self.inst)


def normalize(v):
    n = sum(x * x for x in v) ** 0.5
    return tuple(x / n for x in v)


def dist(a, b) -> float:
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=39570)
    args = ap.parse_args()

    results, info = [], {}
    client = VHMDClient(port=args.port, timeout=2.0)
    xr = None
    try:
        xr = EyeGazeSession()
        xr.open()
        info["openxr_system"] = xr.system_name
        results.append({"name": "OpenXR: SteamVR reports XR_EXT_eye_gaze_interaction support for the vhmd HMD",
                        "ok": xr.supports_eye_gaze, "detail": xr.system_name})

        t0 = time.perf_counter()
        while time.perf_counter() - t0 < 15 and not (xr.state == STATE_FOCUSED and xr.gaze_active()):
            client.gaze(target=(0, 0, -1))
            xr.sync()
            time.sleep(0.02)
        info["openxr_focus_s"] = round(time.perf_counter() - t0, 2)
        results.append({"name": "OpenXR: headless session focused and eye gaze action active",
                        "ok": xr.state == STATE_FOCUSED and xr.gaze_active(), "detail": f"session state {xr.state}"})

        tracked = LOCATION_VALID | LOCATION_TRACKED

        # Head pose in LOCAL vs STAGE: equal while the seated zero pose is at
        # the origin (an uncalibrated universe turned LOCAL upside down).
        head_pos, head_rot = (1.0, 1.6, 2.0), math3d.from_euler(yaw=90)
        t1 = time.perf_counter()
        while time.perf_counter() - t1 < 1.0:
            client.pose("hmd", head_pos, head_rot)
            xr.sync()
            time.sleep(0.02)
        _, _, lp, lq = xr.locate("local", space="view")
        _, _, sp, sq = xr.locate("stage", space="view")
        up_local, up_stage = math3d.rotate(lq, (0, 1, 0)), math3d.rotate(sq, (0, 1, 0))
        results.append({"name": "OpenXR: head pose in LOCAL space matches STAGE (upright, same origin)",
                        "ok": dist(lp, sp) < TOL_M and dist(up_local, up_stage) < 0.002 and dist(sp, head_pos) < TOL_M,
                        "detail": f"local pos {tuple(round(x, 3) for x in lp)} up {tuple(round(x, 2) for x in up_local)}, "
                                  f"stage pos {tuple(round(x, 3) for x in sp)} up {tuple(round(x, 2) for x in up_stage)}"})

        for label, pos, rot, origin, target in CASES:
            # Gaze pose: position = origin, -Z = direction to the target.
            want_dir_view = normalize(tuple(t - o for t, o in zip(target, origin)))
            want_pos_stage = tuple(p + v for p, v in zip(pos, math3d.rotate(rot, origin)))
            want_dir_stage = math3d.rotate(rot, want_dir_view)

            def located():
                got = {}
                for base in ("view", "stage"):
                    r, flags, p, q = xr.locate(base)
                    got[base] = (r, flags, p, math3d.rotate(q, (0, 0, -1)))
                return got

            def matches():
                g = located()
                v, s = g["view"], g["stage"]
                return (v[0] == 0 and s[0] == 0 and v[1] & tracked == tracked and s[1] & tracked == tracked
                        and dist(v[2], origin) < TOL_M and dist(v[3], want_dir_view) < 0.002
                        and dist(s[2], want_pos_stage) < TOL_M and dist(s[3], want_dir_stage) < 0.002)

            ok = False
            t1, last = time.perf_counter(), 0.0
            while time.perf_counter() - t1 < 3.0 and not ok:
                if time.perf_counter() - last > 0.05:
                    client.pose("hmd", pos, rot)
                    client.gaze(origin=origin, target=target)
                    last = time.perf_counter()
                xr.sync()
                ok = matches()
            g = located()
            results.append({"name": f"OpenXR: eye gaze pose in VIEW and STAGE space ({label})", "ok": ok,
                            "detail": f"view dir {tuple(round(x, 3) for x in g['view'][3])} want "
                                      f"{tuple(round(x, 3) for x in want_dir_view)}, stage pos "
                                      f"{tuple(round(x, 3) for x in g['stage'][2])} want "
                                      f"{tuple(round(x, 3) for x in want_pos_stage)}"})

        t1 = time.perf_counter()
        flags = None
        while time.perf_counter() - t1 < 3.0:
            client.gaze(valid=False)
            xr.sync()
            flags = xr.locate("view")[1]
            if flags & tracked == 0:
                break
        results.append({"name": "OpenXR: gaze valid=false clears the location flags", "ok": flags is not None
                        and flags & tracked == 0, "detail": f"flags {flags:#x}" if flags is not None else ""})
    except Skip as e:
        info["openxr_skipped"] = str(e)
    except XrError as e:
        results.append({"name": "OpenXR: headless eye gaze session", "ok": False, "detail": str(e)})
    finally:
        client.gaze(valid=False)
        client.close()
        if xr is not None:
            xr.close()
    print(json.dumps({"results": results, "info": info}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
