"""The physical headset's display as VHMD Link reports it, and the VHMD
settings that make VHMD render for it.

The app sends a "headset" message now and then (inside its pose messages):

    {"type": "headset", "model": "...", "refresh": 72.0, "render": [1920, 1920],
     "fov": [[l, r, u, d], [l, r, u, d]],          # radians, OpenXR signs
     "eyes": [[x, y, z, qw, qx, qy, qz], [...]]}   # eye poses in head space

headset_bridge stores the latest in %LOCALAPPDATA%/VHMD/headset.json and says
when VHMD's projection differs. `python -m vhmd_client.mode vhmd --headset`
then writes the matching settings and restarts SteamVR once:

- SteamVR settings section "vhmd_display": renderWidth / renderHeight (per eye),
  per-eye tangents (leftTanLeft ... rightTanDown), ipdMeters, displayFrequency;
- the VHMD config profile (rebuilt from the installed config on every switch, so
  a driver build that resets the installed config does not lose it): direct
  mode at the headset's refresh rate, and a preview (the video's source) at that
  rate and the headset's eye width.
"""

from __future__ import annotations

import json
import math
import os
import time
from pathlib import Path
from typing import Callable, Dict, Optional

PATH = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "VHMD" / "headset.json"
# Present while VHMD renders for the headset (mode vhmd --headset until --no-headset),
# so later switches keep the headset's settings.
APPLIED = PATH.with_name("headset_applied")
TAN_KEYS = (("leftTanLeft", "leftTanRight", "leftTanUp", "leftTanDown"),
            ("rightTanLeft", "rightTanRight", "rightTanUp", "rightTanDown"))
DISPLAY_KEYS = ("renderWidth", "renderHeight", "ipdMeters", "displayFrequency") + TAN_KEYS[0] + TAN_KEYS[1]

_last_saved: Optional[dict] = None
_last_check = 0.0


def load() -> Optional[dict]:
    try:
        return json.loads(PATH.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def ipd(info: dict) -> float:
    a, b = info["eyes"][0], info["eyes"][1]
    return math.dist(a[:3], b[:3])


def display_settings(info: dict) -> Dict[str, object]:
    """vhmd_display values that make VHMD's projection the headset's."""
    out: Dict[str, object] = {
        "renderWidth": int(info["render"][0]),
        "renderHeight": int(info["render"][1]),
        "ipdMeters": round(min(0.09, max(0.04, ipd(info))), 5),
        "displayFrequency": float(info.get("refresh") or 90.0),
    }
    for eye in (0, 1):
        for key, angle in zip(TAN_KEYS[eye], info["fov"][eye]):
            out[key] = round(math.tan(angle), 6)
    return out


def profile_edits(info: dict) -> Dict[str, Dict[str, str]]:
    """VHMD config values for streaming to the headset (JSON text per key)."""
    hz = float(info.get("refresh") or 90.0)
    return {"display": {"mode": '"direct"', "refreshHz": f"{hz:g}"},
            "preview": {"fps": f"{hz:g}", "maxEyeWidth": str(int(info["render"][0]))}}


def differs(info: dict, fovs) -> bool:
    """True when VHMD's per-eye fields of view (radians, from SteamVR) are not the headset's."""
    if not fovs:
        return False
    return any(abs(a - b) > 0.01 for e in (0, 1) for a, b in zip(info["fov"][e], fovs[e]))


def update(msg: dict, log: Callable[[str], None] = print) -> None:
    """Called by headset_bridge for each "headset" message."""
    global _last_saved, _last_check
    info = {k: msg[k] for k in ("model", "refresh", "render", "fov", "eyes") if k in msg}
    if not all(k in info for k in ("render", "fov", "eyes")):
        return
    if info != _last_saved:
        PATH.parent.mkdir(parents=True, exist_ok=True)
        PATH.write_text(json.dumps(info, indent=1), encoding="utf-8")
        _last_saved = info
        deg = [[round(math.degrees(a), 1) for a in eye] for eye in info["fov"]]
        log(f"headset: {info.get('model', '?')} {info['render'][0]}x{info['render'][1]} per eye, "
            f"{info.get('refresh', 0):g} Hz, IPD {ipd(info) * 1000:.1f} mm, fov (deg) L {deg[0]} R {deg[1]}")
    if time.time() - _last_check > 60:
        _last_check = time.time()
        from .headset_stream import eye_fovs
        if differs(info, eye_fovs()):
            log("headset: VHMD renders with a different field of view; to match the headset run "
                "`python -m vhmd_client.mode vhmd --headset` (restarts SteamVR once)")
