"""What SteamVR gets from VHMD, for the PC app's list. The driver keeps each device's
mode (config op "tracking") until SteamVR ends, so a pause is not saved:

"active" passes the device on, "paused" holds it where it is, connected and
tracked, "disconnected" reports it as off until it is set back.
"""

from __future__ import annotations

from typing import Dict, List, Tuple

ACTIVE, PAUSED, DISCONNECTED = "active", "paused", "disconnected"
SILENT_AFTER = 1.0   # seconds without a pose: the sender has stopped


def state(device: Dict) -> str:
    """"tracking", "silent", "paused" or "disconnected"."""
    mode = device.get("tracking", ACTIVE)
    if mode != ACTIVE:
        return mode
    age = device.get("age_s")
    return "tracking" if age is not None and age < SILENT_AFTER else "silent"


def actions(device: Dict) -> List[Tuple[str, str]]:
    """(button, mode it sets). None for the HMD: the picture is drawn for its pose."""
    if device.get("kind") == "hmd":
        return []
    mode = device.get("tracking", ACTIVE)
    if mode == DISCONNECTED:
        return [("reconnect", ACTIVE)]
    return [("resume", ACTIVE) if mode == PAUSED else ("pause", PAUSED), ("disconnect", DISCONNECTED)]


def rows(devices: List[Dict]) -> List[Dict]:
    """The devices SteamVR has, from the driver's `status`. The name is the role of
    a tracker; a controller's role is only its side."""
    return [{"id": d["id"], "name": d.get("kind") == "tracker" and d.get("role") or d["id"], "state": state(d),
             "rate": round(d.get("rate_hz") or 0), "actions": actions(d)}
            for d in devices if d.get("index", -1) >= 0]
