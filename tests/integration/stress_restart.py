"""SteamVR start/stop endurance test (requirement: 100 cycles without crash/hang).

Each cycle: start SteamVR -> wait for the driver's UDP ping -> check all
configured devices got a SteamVR index -> send a burst of poses -> stop
SteamVR -> confirm from vrserver.txt that the driver's Cleanup() ran and
vrserver exited gracefully, with no crash/exception lines.

    python tests/integration/stress_restart.py [--cycles 100] [--stop-relaunchers]

Results are appended to tests/integration/results/stress_<timestamp>.json
after every cycle, so an interrupted run still leaves a record.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "clients" / "python"))
sys.path.insert(0, str(HERE))

import steamvr_util as svr  # noqa: E402
from vhmd_client import VHMDClient  # noqa: E402

# Driver errors and crash indicators. SteamVR's "[Input] <app> (vhmd_tracker)
# has no configured binding" notices concern apps, not the driver, and are excluded.
CRASH_PATTERNS = (r"(?i)(unhandled exception|crashed|crash ?dump|minidump|access violation|exception code"
                  r"|vhmd: \[error\]|\[Error\].*driver vhmd|\[Remapping\].*failed)")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cycles", type=int, default=30)
    ap.add_argument("--port", type=int, default=39570)
    ap.add_argument("--stop-relaunchers", action="store_true",
                    help="terminate OpenVR overlays that relaunch SteamVR (see steamvr_util.stop_steamvr)")
    ap.add_argument("--relaunch-watch", type=float, default=8.0,
                    help="seconds to watch for an unwanted SteamVR relaunch after each stop (2 is enough with --stop-relaunchers)")
    ap.add_argument("--settle", type=float, default=2.0, help="seconds to wait after the driver answers")
    args = ap.parse_args()

    out = HERE / "results" / f"stress_{datetime.now():%Y%m%d_%H%M%S}.json"
    out.parent.mkdir(exist_ok=True)
    cycles = []

    if svr.running(("vrserver.exe",)):
        svr.stop_steamvr(stop_relaunchers=args.stop_relaunchers)

    for i in range(1, args.cycles + 1):
        rec = {"cycle": i, "ok": False}
        t0 = time.monotonic()
        try:
            svr.start_steamvr()
            client = VHMDClient(port=args.port, timeout=0.5)
            while not client.ping():
                if time.monotonic() - t0 > 120:
                    raise TimeoutError("driver did not answer within 120 s")
            rec["driver_up_s"] = round(time.monotonic() - t0, 2)
            pid = svr.vrserver_pid()
            time.sleep(args.settle)
            st = client.status()
            # On-demand trackers only get an index once something is sent to them.
            enabled = [d for d in st["devices"] if d["enabled"] and not d.get("on_demand")]
            rec["devices_indexed"] = sum(d["index"] >= 0 for d in enabled)
            rec["devices_enabled"] = len(enabled)
            for k in range(200):  # some traffic during the session
                client.pose("hmd", (0.0, 1.6 + 0.001 * k, 0.0))
            client.close()

            rec["stop_s"] = round(svr.stop_steamvr(stop_relaunchers=args.stop_relaunchers,
                                                   relaunch_watch=args.relaunch_watch), 2)
            # The session of the vrserver this cycle started (found by PID).
            log = svr.session_by_pid(pid) if pid else svr.latest_session(svr.read_log_since(svr.vrserver_log(), 0))
            rec["cleanup_logged"] = bool(svr.log_lines(log, r"vhmd: shutting down"))
            rec["graceful_exit"] = bool(svr.log_lines(log, r"\[vrserver\] Graceful exit"))
            rec["problems"] = [l.strip()[:200] for l in svr.log_lines(log, CRASH_PATTERNS)][:5]
            rec["ok"] = (rec["devices_indexed"] == rec["devices_enabled"] and rec["cleanup_logged"]
                         and rec["graceful_exit"] and not rec["problems"])
        except Exception as e:  # record and keep going
            rec["error"] = f"{type(e).__name__}: {e}"
            try:
                svr.stop_steamvr(stop_relaunchers=args.stop_relaunchers)
            except Exception as e2:
                rec["error"] += f" / stop failed: {e2}"
        rec["total_s"] = round(time.monotonic() - t0, 1)
        cycles.append(rec)
        passed = sum(c["ok"] for c in cycles)
        print(f"cycle {i:3}/{args.cycles}: {'OK ' if rec['ok'] else 'FAIL'} "
              f"up={rec.get('driver_up_s')}s stop={rec.get('stop_s')}s {rec.get('error', '')}", flush=True)
        out.write_text(json.dumps({"cycles_run": i, "passed": passed, "cycles": cycles}, indent=1), encoding="utf-8")

    passed = sum(c["ok"] for c in cycles)
    print(f"\n{passed}/{len(cycles)} cycles OK  report: {out}")
    return 0 if passed == len(cycles) else 1


if __name__ == "__main__":
    sys.exit(main())
