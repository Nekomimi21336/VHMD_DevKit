"""Position and angle corrections of controllers and trackers. The driver applies
them (config op "offset") but forgets them when SteamVR ends, so the PC app keeps
them and sends them again.

A correction is {"pos": [x, y, z] in meters, "ypr": [yaw, pitch, roll] in degrees}.
Angles turn around the device's own axes. Positions move along the controller's
own axes (what "the grip sits 2 cm further forward" means) and along the room's
for a tracker (what "the foot is 3 cm too high" means), whichever way it faces.
"""

from __future__ import annotations

from typing import Dict, List, Tuple

MAX_METERS = 2.0
MAX_DEGREES = 180.0
ZERO = {"pos": [0.0, 0.0, 0.0], "ypr": [0.0, 0.0, 0.0]}
HANDS = ("left_hand", "right_hand")


def space(device_id: str) -> str:
    return "device" if device_id in HANDS else "room"


def clean(value: Dict) -> Dict:
    """Within the limits the driver accepts."""
    pos = [max(-MAX_METERS, min(MAX_METERS, float(v))) for v in value.get("pos", ZERO["pos"])]
    ypr = [max(-MAX_DEGREES, min(MAX_DEGREES, float(v))) for v in value.get("ypr", ZERO["ypr"])]
    return {"pos": pos, "ypr": ypr}


def is_zero(value: Dict) -> bool:
    return not any(value["pos"]) and not any(value["ypr"])


def message(device_id: str, value: Dict) -> Dict:
    return {"id": device_id, **clean(value), "space": space(device_id)}


def pending(devices: List[Dict], wanted: Dict[str, Dict]) -> List[Dict]:
    """What to send for the driver's `status` devices to match `wanted`."""
    out = []
    for d in devices:
        if d.get("kind") not in ("controller", "tracker"):
            continue
        want = message(d["id"], wanted.get(d["id"], ZERO))
        have = d.get("offset")
        if have is None:
            have = {**ZERO, "space": want["space"]}   # the driver leaves out a zero one
        same = all(abs(a - b) < 1e-6 for key in ("pos", "ypr") for a, b in zip(want[key], have[key]))
        if not same or (have["space"] != want["space"] and not is_zero(want)):
            out.append(want)
    return out


def choices(devices: List[Dict]) -> List[Tuple[str, str]]:
    """(id, name) of the controllers and of the trackers SteamVR has. The name is
    the role of a tracker; the hands are always there."""
    out = [(h, h) for h in HANDS]
    for d in devices:
        if d.get("kind") == "tracker" and d.get("index", -1) >= 0:
            out.append((d["id"], d.get("role") or d["id"]))
    return out
