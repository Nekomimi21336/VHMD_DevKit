"""Checks config op "display": changing the HMD's render size, field of view and
IPD while SteamVR runs, read back from SteamVR, and the way back.

Needs SteamVR running with VHMD's HMD. Connects as a background app (nothing is
rendered). Whatever happens, the display is reset to the values SteamVR started
with before the script ends. Prints one JSON line: {"results": [...], "info": {...}}.

    python tests/integration/display_check.py [--port 39570] [--wait 5]
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "clients" / "python"))
sys.path.insert(0, str(HERE))

import openvr  # noqa: E402

from vhmd_client import VHMDClient  # noqa: E402

EYES = (openvr.Eye_Left, openvr.Eye_Right)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=39570)
    ap.add_argument("--wait", type=float, default=5.0, help="seconds to wait for SteamVR to show a change")
    args = ap.parse_args()

    results, info = [], {}

    def check(name, ok, detail=""):
        results.append({"name": name, "ok": bool(ok), "detail": detail})

    system = openvr.init(openvr.VRApplication_Background)
    client = VHMDClient(port=args.port, timeout=2.0)

    def steamvr():
        """What SteamVR has now: [left, right] tangents (OpenXR signs), the eyes'
        x in the head, and the size it tells apps to render."""
        tans, xs = [], []
        for eye in EYES:
            left, right, top, bottom = system.getProjectionRaw(eye)
            tans.append([left, right, -top, -bottom])
            xs.append(system.getEyeToHeadTransform(eye)[0][3])
        w, h = system.getRecommendedRenderTargetSize()
        return {"tan": tans, "eye_x": xs, "render": [w, h]}

    def wait_for(pred):
        t0 = time.perf_counter()
        while time.perf_counter() - t0 < args.wait:
            ev = openvr.VREvent_t()
            while system.pollNextEvent(ev):
                if ev.eventType in (openvr.VREvent_IpdChanged, openvr.VREvent_LensDistortionChanged,
                                    openvr.VREvent_PropertyChanged):
                    info.setdefault("events", []).append(system.getEventTypeNameFromEnum(ev.eventType))
            if pred(steamvr()):
                return round(time.perf_counter() - t0, 2)
            time.sleep(0.05)
        return None

    def close(a, b, tol):
        return all(abs(x - y) <= tol for x, y in zip(a, b))

    try:
        d = client.display()
        start, base = d["now"], steamvr()
        info["driver_startup"], info["driver_before"], info["steamvr_before"] = d["startup"], start, base
        check("display: query shows startup and current values", "startup" in d and d["live"] is True, json.dumps(d))
        check("SteamVR's field of view is the driver's",
              all(close(base["tan"][e], start["tan"][e], 1e-3) for e in (0, 1)), f"{base['tan']} vs {start['tan']}")
        check("SteamVR's eyes are half the IPD to each side",
              close(base["eye_x"], [-start["ipd"] / 2, start["ipd"] / 2], 2e-4), f"{base['eye_x']} vs ipd {start['ipd']}")

        # IPD: 4 mm narrower (or wider, near the lower limit).
        ipd = start["ipd"] - 0.004 if start["ipd"] > 0.046 else start["ipd"] + 0.004
        client.display(ipd=ipd)
        t = wait_for(lambda s: close(s["eye_x"], [-ipd / 2, ipd / 2], 2e-4))
        check("IPD change reaches SteamVR while it runs", t is not None, f"{t} s, eyes {steamvr()['eye_x']}, wanted +-{ipd / 2:.5f}")

        # Field of view: 10% narrower, both eyes.
        tan = [[round(x * 0.9, 6) for x in eye] for eye in start["tan"]]
        client.display(tan=tan)
        t = wait_for(lambda s: all(close(s["tan"][e], tan[e], 1e-3) for e in (0, 1)))
        check("field of view change reaches SteamVR while it runs", t is not None, f"{t} s, {steamvr()['tan']}")

        # Render size: three quarters. SteamVR scales what it tells apps, so the
        # ratio is what has to follow.
        render = [max(64, int(start["render"][0] * 0.75) // 16 * 16), max(64, int(start["render"][1] * 0.75) // 16 * 16)]
        want = render[0] / start["render"][0]
        client.display(render=render)
        t = wait_for(lambda s: abs(s["render"][0] / base["render"][0] - want) < 0.03)
        after = steamvr()
        check("render size change reaches SteamVR while it runs", t is not None,
              f"{t} s, apps get {after['render']} (was {base['render']}), driver {render} (was {start['render']})")
        st = client.status()["display"]
        check("status shows the new size", [st["width"], st["height"]] == render, f"{st['width']}x{st['height']}")
        mid = client.display()
        check("display: reports the change next to the startup values",
              mid["changed"] is True and mid["startup"] == d["startup"] and mid["now"]["render"] == render, json.dumps(mid["now"]))
        info["steamvr_changed"] = after

        # Out of range: refused, nothing changes.
        try:
            client.display(ipd=0.2)
            refused = False
        except ValueError:
            refused = True
        check("out-of-range values are refused", refused and client.display()["now"] == mid["now"])
    finally:
        # Always back to how SteamVR started.
        try:
            back = client.display(reset=True)
            t = wait_for(lambda s: close(s["eye_x"], info["steamvr_before"]["eye_x"], 2e-4)
                         and all(close(s["tan"][e], info["steamvr_before"]["tan"][e], 1e-3) for e in (0, 1))
                         and s["render"] == info["steamvr_before"]["render"])
            info["steamvr_after_reset"] = steamvr()
            check("reset: the driver is back at the startup values", back["changed"] is False and back["now"] == back["startup"])
            check("reset: SteamVR is back where it was", t is not None, f"{t} s, {info['steamvr_after_reset']}")
        except Exception as e:  # noqa: BLE001 (reported, not raised: the results line must come out)
            check("reset", False, repr(e))
        client.close()
        openvr.shutdown()

    print(json.dumps({"results": results, "info": info}))
    return 0 if all(r["ok"] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
