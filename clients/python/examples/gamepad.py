"""Controller input demo.

With pygame installed and a gamepad connected, the gamepad drives the virtual
controllers (sticks -> joysticks, triggers -> triggers, shoulder -> grip,
face buttons -> A/B/X/Y). Without a gamepad (or with --demo) a scripted input
sequence is played instead, and finger curls follow the inputs.

    python gamepad.py [--demo]
"""

import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from vhmd_client import VHMDClient  # noqa: E402


def scripted(t: float):
    """Yields (device, values) for a looping 8 second demo."""
    phase = t % 8.0
    trig = max(0.0, math.sin(math.pi * phase / 2)) if phase < 2 else 0.0
    grip = max(0.0, math.sin(math.pi * (phase - 2) / 2)) if 2 <= phase < 4 else 0.0
    jx = math.cos(2 * math.pi * phase / 2) if 4 <= phase < 6 else 0.0
    jy = math.sin(2 * math.pi * phase / 2) if 4 <= phase < 6 else 0.0
    btn = 6 <= phase < 7
    for dev in ("left_hand", "right_hand"):
        yield dev, {
            "trigger": round(trig, 3),
            "grip": round(grip, 3),
            "joystick_x": round(jx, 3),
            "joystick_y": round(jy, 3),
            "joystick_touch": 4 <= phase < 6,
            "primary": btn,
            "secondary": 7 <= phase < 8,
        }


def from_pygame(js):
    import pygame  # noqa: F401  (imported by caller)

    def axis(i):
        return js.get_axis(i) if i < js.get_numaxes() else 0.0

    def button(i):
        return bool(js.get_button(i)) if i < js.get_numbuttons() else False

    # SDL "XInput" layout: axes 0/1 left stick, 2/3 right stick, 4/5 triggers (-1..1)
    lt = (axis(4) + 1) / 2
    rt = (axis(5) + 1) / 2
    yield "left_hand", {
        "joystick_x": axis(0), "joystick_y": -axis(1), "trigger": lt,
        "grip": 1.0 if button(4) else 0.0, "x": button(2), "y": button(3),
        "joystick_click": button(8), "menu": button(6),
    }
    yield "right_hand", {
        "joystick_x": axis(2), "joystick_y": -axis(3), "trigger": rt,
        "grip": 1.0 if button(5) else 0.0, "a": button(0), "b": button(1),
        "joystick_click": button(9), "system": button(7),
    }


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=39570)
    ap.add_argument("--rate", type=float, default=90.0)
    ap.add_argument("--demo", action="store_true", help="scripted input even if a gamepad is present")
    ap.add_argument("--duration", type=float, default=0.0)
    args = ap.parse_args()

    js = None
    if not args.demo:
        try:
            import pygame

            pygame.init()
            pygame.joystick.init()
            if pygame.joystick.get_count() > 0:
                js = pygame.joystick.Joystick(0)
                js.init()
                print(f"using gamepad: {js.get_name()}")
        except ImportError:
            print("pygame not installed; running the scripted demo (pip install pygame for gamepad input)")
    if js is None:
        print("scripted input demo, Ctrl+C to stop")

    with VHMDClient(args.host, args.port) as c:
        t0 = time.perf_counter()
        next_t = t0
        try:
            while args.duration <= 0 or time.perf_counter() - t0 < args.duration:
                if js is not None:
                    import pygame

                    pygame.event.pump()
                    source = from_pygame(js)
                else:
                    source = scripted(time.perf_counter() - t0)
                with c.batch():
                    for dev, values in source:
                        c.inputs(dev, {k: (float(v) if not isinstance(v, bool) else v) for k, v in values.items()})
                for h in c.poll_haptics():
                    print(f"haptic {h['id']}: amp={h['amplitude']:.2f} dur={h['duration']:.3f}s")
                next_t += 1.0 / args.rate
                time.sleep(max(0.0, next_t - time.perf_counter()))
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
