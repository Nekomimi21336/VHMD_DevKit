"""VHMD Link under load in the PC app's process, without a headset.

Runs headset_bridge.Bridge in this process (as the PC app does), a busy thread
that now and then holds Python's interpreter lock for a long time (as a busy UI
or a garbage collection can), and link/sim (vhmd-link-sim) as the headset; then
prints what the "headset" saw. Run once with the Python data path and once with
the native relay to compare:

    python tests/integration/link_load_test.py --native off --seconds 45
    python tests/integration/link_load_test.py --native on  --seconds 45
    python tests/integration/link_load_test.py --native on  --no-load

Needs SteamVR running in VHMD mode (python -m vhmd_client.mode vhmd), the link
built (cargo build --release in link/), and no other VHMD Link running (the
bridge listens on 127.0.0.1 only, so a headset on Wi-Fi does not join in).
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "clients" / "python"))

from vhmd_client.headset_bridge import Bridge, BridgeOptions  # noqa: E402

SIM = ROOT / "link" / "target" / "release" / ("vhmd-link-sim.exe" if sys.platform == "win32" else "vhmd-link-sim")


def busy(stop: threading.Event, hold_ms: float) -> None:
    """Python work that holds the interpreter lock in long stretches: sorting a
    big list is one C call (the lock is not released) of about hold_ms."""
    n = int(25_000 * hold_ms)
    data = list(range(n, 0, -1))
    while not stop.is_set():
        sorted(data)                       # one long hold
        for _ in range(20_000):            # ordinary Python work
            pass
        time.sleep(0.05)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--native", choices=("on", "off"), default="on")
    ap.add_argument("--seconds", type=float, default=45)
    ap.add_argument("--no-load", action="store_true")
    ap.add_argument("--hold-ms", type=float, default=40, help="about how long each busy stretch holds the lock")
    args = ap.parse_args()
    if not SIM.exists():
        raise SystemExit(f"build the link first: cargo build --release (in {ROOT / 'link'})")

    bridge = Bridge(BridgeOptions(usb_only=True, adb=False, native=args.native), log=lambda m: None)
    bridge.start()
    stop = threading.Event()
    if not args.no_load:
        threading.Thread(target=busy, args=(stop, args.hold_ms), daemon=True).start()
    fwd_max, relay_max = 0.0, 0.0
    out = Path(tempfile.gettempdir()) / f"link_load_{args.native}.json"
    sim = subprocess.Popen([str(SIM), "--pc", "127.0.0.1", "--usb", "--seconds", str(args.seconds), "--quiet",
                            "--json", str(out)], stdout=subprocess.DEVNULL)
    while sim.poll() is None:
        time.sleep(1.0)
        st = bridge.status()
        fwd_max = max(fwd_max, st["forward_us"][1])
        relay_max = max(relay_max, st["relay_ms"][2])
    stop.set()
    bridge.close()
    r = json.loads(out.read_text())
    print(f"data path {bridge.data_path}, load {'off' if args.no_load else f'on (~{args.hold_ms:g} ms holds)'}: "
          f"shown {r['shown_per_s']:.1f}/s, repeats {r['repeats']}, dropped {r['dropped']}, lost {r['lost']} | "
          f"pose->frame {r['pose_to_frame_ms']:.1f} ms (max {r['pose_to_frame_max_ms']:.1f}) | "
          f"transit {r['transit_ms']:.2f} ms (max {r['transit_max_ms']:.1f}) | "
          f"pose forwarding max {fwd_max / 1000:.2f} ms | wait {r['wait_ms']:.1f} ms")


if __name__ == "__main__":
    main()
