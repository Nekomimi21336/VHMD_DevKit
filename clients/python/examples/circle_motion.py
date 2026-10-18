"""Move the virtual HMD on a horizontal circle while it looks at the center.

    python circle_motion.py [--radius 0.5] [--period 4] [--rate 90]
"""

import argparse
import math
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from vhmd_client import VHMDClient, math3d  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=39570)
    ap.add_argument("--radius", type=float, default=0.5)
    ap.add_argument("--height", type=float, default=1.7)
    ap.add_argument("--period", type=float, default=4.0, help="seconds per revolution")
    ap.add_argument("--rate", type=float, default=90.0)
    ap.add_argument("--duration", type=float, default=0.0, help="0 = run until Ctrl+C")
    args = ap.parse_args()

    w = 2 * math.pi / args.period
    with VHMDClient(args.host, args.port) as c:
        t0 = time.perf_counter()
        next_t = t0
        print("sending HMD circle motion, Ctrl+C to stop")
        try:
            while args.duration <= 0 or time.perf_counter() - t0 < args.duration:
                t = time.perf_counter() - t0
                a = w * t
                pos = (args.radius * math.sin(a), args.height, args.radius * math.cos(a))
                vel = (args.radius * w * math.cos(a), 0.0, -args.radius * w * math.sin(a))
                # Face the circle center: forward (-Z) rotated by yaw `a` points
                # to (-sin a, 0, -cos a), i.e. from pos towards the origin.
                rot = math3d.from_euler(yaw=a, degrees=False)
                c.pose("hmd", pos, rot, vel=vel, angvel=(0.0, w, 0.0))
                next_t += 1.0 / args.rate
                time.sleep(max(0.0, next_t - time.perf_counter()))
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
