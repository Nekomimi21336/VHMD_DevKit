"""Drive HMD, both controllers and 8 trackers with a swaying / stepping body.

All 11 poses are sent in one JSON array per frame (one UDP datagram).

    python fullbody_sway.py [--height 1.7] [--rate 90] [--no-hmd]
"""

import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from vhmd_client import VHMDClient, math3d  # noqa: E402


def body_frame(t: float, height: float):
    """Returns {id: (pos, rot)} for a person facing -Z, swaying and stepping."""
    s = height / 1.7
    sway = 0.06 * math.sin(2 * math.pi * 0.5 * t)  # lateral hip sway
    bob = 0.02 * math.sin(2 * math.pi * 1.0 * t)
    yaw = math.radians(10) * math.sin(2 * math.pi * 0.25 * t)
    step_l = max(0.0, math.sin(2 * math.pi * 0.5 * t)) * 0.10 * s  # foot lift
    step_r = max(0.0, -math.sin(2 * math.pi * 0.5 * t)) * 0.10 * s
    q_yaw = math3d.from_euler(yaw=yaw, degrees=False)

    def body(p, extra=math3d.IDENTITY):
        # rotate body-local offsets by the body yaw around the hip
        x, y, z = math3d.rotate(q_yaw, p)
        return (x + sway, y + bob, z), math3d.multiply(q_yaw, extra)

    arm = math.radians(25) * math.sin(2 * math.pi * 0.5 * t)
    frame = {
        "hmd": body((0.0, 1.70 * s - 0.0, 0.0), math3d.from_euler(pitch=-5)),
        "left_hand": body((-0.25 * s, 1.05 * s, -0.25 * s), math3d.from_euler(pitch=math.degrees(arm))),
        "right_hand": body((0.25 * s, 1.05 * s, -0.25 * s), math3d.from_euler(pitch=-math.degrees(arm))),
        "tracker_0": body((0.0, 1.00 * s, 0.0)),                                        # waist
        "tracker_1": body((-0.12 * s, 0.08 * s + step_l, -step_l * 0.5)),               # left foot
        "tracker_2": body((0.12 * s, 0.08 * s + step_r, -step_r * 0.5)),                # right foot
        "tracker_3": body((-0.11 * s, 0.50 * s + step_l * 0.6, -0.03 * s - step_l)),    # left knee
        "tracker_4": body((0.11 * s, 0.50 * s + step_r * 0.6, -0.03 * s - step_r)),     # right knee
        "tracker_5": body((-0.30 * s, 1.15 * s, 0.02 * s)),                             # left elbow
        "tracker_6": body((0.30 * s, 1.15 * s, 0.02 * s)),                              # right elbow
        "tracker_7": body((0.0, 1.35 * s, -0.05 * s)),                                  # chest
    }
    return frame


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=39570)
    ap.add_argument("--height", type=float, default=1.7)
    ap.add_argument("--rate", type=float, default=90.0)
    ap.add_argument("--duration", type=float, default=0.0, help="0 = run until Ctrl+C")
    ap.add_argument("--no-hmd", action="store_true", help="do not send the HMD pose (use with a real headset)")
    args = ap.parse_args()

    with VHMDClient(args.host, args.port) as c:
        t0 = time.perf_counter()
        next_t = t0
        prev = None
        print("sending full body motion (HMD + 2 controllers + 8 trackers), Ctrl+C to stop")
        try:
            while args.duration <= 0 or time.perf_counter() - t0 < args.duration:
                t = time.perf_counter() - t0
                frame = body_frame(t, args.height)
                dt = 1.0 / args.rate
                with c.batch():
                    for dev, (pos, rot) in frame.items():
                        if args.no_hmd and dev == "hmd":
                            continue
                        vel = None
                        if prev:
                            p0 = prev[dev][0]
                            vel = tuple((a - b) / dt for a, b in zip(pos, p0))
                        c.pose(dev, pos, rot, vel=vel)
                prev = frame
                next_t += dt
                time.sleep(max(0.0, next_t - time.perf_counter()))
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
