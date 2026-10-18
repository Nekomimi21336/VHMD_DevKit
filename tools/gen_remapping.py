"""Generates driver/resources/input/vhmd_controller_remapping.json.

The remapping lets SteamVR convert an application's bindings for other
controller types into bindings for "vhmd_controller". simulate_* are false:
apps always see the real controller type ("vhmd_controller"), nothing is
impersonated. Every "from" path must exist in the source controller's input
profile (SteamVR rejects the whole file otherwise), so the source lists below
mirror the profiles shipped with SteamVR 2.17.10:
  oculus_touch:    drivers/oculus/resources/input/touch_profile.json
  knuckles:        drivers/indexcontroller/resources/input/index_controller_profile.json
  vive_controller: drivers/htc/resources/input/vive_controller_profile.json
"""

import json
from pathlib import Path

OUT = Path(__file__).resolve().parents[1] / "driver" / "resources" / "input" / "vhmd_controller_remapping.json"


def both(src: str, dst: str):
    return [(f"/user/hand/{s}/input/{src}", f"/user/hand/{s}/input/{dst}") for s in ("left", "right")]


TOUCH = (
    both("trigger", "trigger") + both("grip", "grip") + both("joystick", "joystick")
    + [("/user/hand/left/input/x", "/user/hand/left/input/x"), ("/user/hand/left/input/y", "/user/hand/left/input/y"),
       ("/user/hand/right/input/a", "/user/hand/right/input/a"), ("/user/hand/right/input/b", "/user/hand/right/input/b"),
       ("/user/hand/left/input/system", "/user/hand/left/input/menu")]
)
KNUCKLES = (
    both("trigger", "trigger") + both("grip", "grip") + both("thumbstick", "joystick") + both("system", "system")
    + [("/user/hand/left/input/a", "/user/hand/left/input/x"), ("/user/hand/left/input/b", "/user/hand/left/input/y"),
       ("/user/hand/right/input/a", "/user/hand/right/input/a"), ("/user/hand/right/input/b", "/user/hand/right/input/b")]
)
VIVE = (
    both("trigger", "trigger") + both("grip", "grip") + both("trackpad", "joystick") + both("system", "system")
    + [("/user/hand/left/input/application_menu", "/user/hand/left/input/y"),
       ("/user/hand/right/input/application_menu", "/user/hand/right/input/b")]
)


def main() -> None:
    doc = {"to_controller_type": "vhmd_controller", "layouts": []}
    for prio, src, pairs in ((100, "oculus_touch", TOUCH), (50, "knuckles", KNUCKLES), (10, "vive_controller", VIVE)):
        doc["layouts"].append({
            "priority": prio,
            "from_controller_type": src,
            "simulate_controller_type": False,
            "simulate_render_model": False,
            "simulate_HMD": False,
            "autoremappings": [{"from": f, "to": t} for f, t in pairs],
        })
    OUT.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
