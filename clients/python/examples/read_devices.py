"""Reads what VHMD's devices hand SteamVR, from your own program: the pose,
buttons and axes, finger bones and vibrations of the headset, the controllers
or trackers, whoever sends them (VHMD Link, a script, VMT).

It asks the driver ("inspect") about 30 times a second; asking does not take
anything away from the sender (haptic events still go to it).

    python examples/read_devices.py                          # print hmd and hands
    python examples/read_devices.py --csv out.csv --seconds 20 # write a CSV
"""

from __future__ import annotations

import argparse
import csv
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from vhmd_client import VHMDClient, math3d  # noqa: E402


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--devices", nargs="+", default=["hmd", "left_hand", "right_hand"])
    ap.add_argument("--rate", type=float, default=30.0)
    ap.add_argument("--seconds", type=float, default=0, help="stop after this long (default: Ctrl+C)")
    ap.add_argument("--csv", help="write rows here instead of printing")
    args = ap.parse_args()

    out = None
    if args.csv:
        f = open(args.csv, "w", newline="", encoding="utf-8")
        out = csv.writer(f)
        out.writerow(["time", "device", "x", "y", "z", "yaw", "pitch", "roll", "valid", "inputs"])
    t0 = time.monotonic()
    with VHMDClient(timeout=0.5) as c:
        try:
            while not args.seconds or time.monotonic() - t0 < args.seconds:
                for dev in args.devices:
                    r = c.inspect(dev)
                    # A controller and its hand twin are two SteamVR devices: take the connected one.
                    d = next((x for x in r["devices"] if x.get("active")), r["devices"][0])
                    p, q = d["pose"]["pos"], d["pose"]["rot"]
                    yaw, pitch, roll = math3d.to_euler(q)
                    pressed = {k: round(v, 2) for k, v in d.get("given", {}).items() if v}
                    if out:
                        out.writerow([round(time.monotonic() - t0, 3), dev, *(round(v, 4) for v in p),
                                      round(yaw, 2), round(pitch, 2), round(roll, 2), d["pose"]["valid"], pressed])
                    else:
                        print(f"{dev:11s} ({p[0]:+.3f}, {p[1]:+.3f}, {p[2]:+.3f}) yaw {yaw:+6.1f}  {pressed}")
                time.sleep(1.0 / args.rate)
        except KeyboardInterrupt:
            pass
    if out:
        f.close()


if __name__ == "__main__":
    main()
