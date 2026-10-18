"""Long-running stability test (soak) for the vhmd driver. Windows and Linux.

Streams HMD + both controllers + on-demand trackers at 90 Hz with a slow
sway for --minutes while acting as an OpenVR Scene app (waitGetPoses every
frame), and samples every --interval seconds:

  - Scene app frame rate and how far the right controller pose seen by the
    app is from what was sent
  - driver status: presents/s (direct mode), rejected packets, device ages
  - vrserver memory (RSS / working set)
  - new "vhmd:" warnings / errors in vrserver.txt

    python tests/integration/soak_test.py --minutes 30

Writes tests/integration/results/soak_<time>.json and .md. SteamVR is
started if needed and stopped at the end only if this script started it.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import subprocess
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "clients" / "python"))
sys.path.insert(0, str(HERE))

import openvr  # noqa: E402

import steamvr_util as svr  # noqa: E402
from vhmd_client import VHMDClient, math3d  # noqa: E402

TRACKERS = ["tracker_0", "tracker_1", "tracker_2"]  # waist, feet (added on their first pose)


class Sender(threading.Thread):
    """Sends a slow sway (0.2 Hz, 10 cm) for all devices; remembers what it sent."""

    def __init__(self, client: VHMDClient, trackers):
        super().__init__(daemon=True)
        self.client, self.trackers = client, trackers
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.right = (0.0, 0.0, 0.0)
        self.sent = 0

    def run(self):
        t0 = time.perf_counter()
        nxt = t0
        while not self.stop.is_set():
            t = time.perf_counter() - t0
            s = 0.10 * math.sin(2 * math.pi * 0.2 * t)
            yaw = 20 * math.sin(2 * math.pi * 0.1 * t)
            c = self.client
            c.pose("hmd", (s, 1.6, 0.0), math3d.from_euler(yaw=yaw))
            c.pose("left_hand", (-0.25 + s, 1.1, -0.3), math3d.from_euler(pitch=-30))
            right = (0.25 + s, 1.1, -0.3)
            c.pose("right_hand", right, math3d.from_euler(pitch=-30))
            for i, tid in enumerate(self.trackers):
                c.pose(tid, (s + 0.12 * (i - 1), 1.0 if i == 0 else 0.08, 0.0))
            # A button press once a second keeps the input path busy too.
            c.input("right_hand", "trigger", 1.0 if int(t) % 2 else 0.0)
            with self.lock:
                self.right = right
                self.sent += 3 + len(self.trackers) + 1
            nxt += 1 / 90
            delay = nxt - time.perf_counter()
            if delay > 0:
                time.sleep(delay)
            else:
                nxt = time.perf_counter()


def vrserver_memory_mb() -> float | None:
    if svr.IS_WINDOWS:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq vrserver.exe", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True).stdout
        m = re.search(r'"([\d,.\s]+) K"', out)
        return round(int(re.sub(r"\D", "", m.group(1))) / 1024, 1) if m else None
    for pid, name in svr._process_table():
        if name == "vrserver":
            try:
                for line in Path(f"/proc/{pid}/status").read_text().splitlines():
                    if line.startswith("VmRSS:"):
                        return round(int(line.split()[1]) / 1024, 1)
            except OSError:
                return None
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--minutes", type=float, default=30.0)
    ap.add_argument("--interval", type=float, default=30.0, help="seconds between samples")
    ap.add_argument("--port", type=int, default=39570)
    ap.add_argument("--min-fps", type=float, default=None,
                    help="lowest acceptable Scene app / present rate (default 80 in direct mode, 60 in window mode)")
    ap.add_argument("--max-pose-error-cm", type=float, default=3.0)
    ap.add_argument("--max-memory-growth-mb", type=float, default=50.0)
    args = ap.parse_args()
    if svr.IS_WINDOWS:
        # Default timer resolution is 15.6 ms; without this the 90 Hz sender
        # and this loop are paced to ~64 Hz by time.sleep() alone.
        import ctypes
        ctypes.windll.winmm.timeBeginPeriod(1)

    started_here = False
    if not svr.running(("vrserver",)):
        svr.start_steamvr()
        started_here = True
    client = VHMDClient(port=args.port, timeout=1.0)
    t0 = time.time()
    while not client.ping():
        if time.time() - t0 > 120:
            print("driver did not answer within 120 s")
            return 2
        time.sleep(1)
    time.sleep(3)
    log_path = svr.vrserver_log()
    log_offset = svr.log_size(log_path)

    sender = Sender(client, TRACKERS)
    sender.start()
    for attempt in range(60):  # SteamVR Home may still be launching right after start-up
        try:
            system = openvr.init(openvr.VRApplication_Scene)
            break
        except openvr.error_code.InitError_Init_AnotherAppLaunching:
            time.sleep(1)
    else:
        print("could not connect as a Scene app (another app kept launching)")
        return 2
    compositor = openvr.VRCompositor()
    overlay = openvr.VROverlay()
    poses = (openvr.TrackedDevicePose_t * openvr.k_unMaxTrackedDeviceCount)()

    samples, problems = [], []
    start = time.monotonic()
    end = start + args.minutes * 60
    next_sample = start + args.interval
    frames, worst_err, last_presents, last_rejected = 0, 0.0, None, None
    st = client.status()
    last_presents = st.get("display", {}).get("presents")
    mode = st.get("display", {}).get("mode")
    if args.min_fps is None:
        # Window mode (Windows) is paced by the desktop compositor: about 66 fps was
        # measured on a 200 Hz monitor even with a 90 Hz HMD (docs/verification.md).
        args.min_fps = 60.0 if mode == "window" else 80.0
    last_rejected = st.get("packets", {}).get("rejected", 0)
    last_t = time.monotonic()
    print(f"soak: {args.minutes:g} min, sample every {args.interval:g} s", flush=True)
    while time.monotonic() < end:
        compositor.waitGetPoses(poses, None)
        frames += 1
        if frames % 90 == 0 and overlay.isDashboardVisible():
            subprocess.run([str(svr.vrcmd_path()), "--hidedashboard"], capture_output=True, timeout=10)
        ri = system.getTrackedDeviceIndexForControllerRole(openvr.TrackedControllerRole_RightHand)
        if ri != openvr.k_unTrackedDeviceIndexInvalid and poses[ri].bPoseIsValid:
            m = poses[ri].mDeviceToAbsoluteTracking
            with sender.lock:
                want = sender.right
            err = math.dist((m[0][3], m[1][3], m[2][3]), want)
            worst_err = max(worst_err, err)
        now = time.monotonic()
        if now >= next_sample:
            st = client.status()
            dt = now - last_t
            disp = st.get("display", {})
            presents = disp.get("presents")
            rejected = st.get("packets", {}).get("rejected", 0)
            ages = [d.get("age_s") for d in st.get("devices", []) if d.get("age_s") is not None]
            new_log = svr.read_log_since(log_path, log_offset)
            log_offset = svr.log_size(log_path)
            vhmd_bad = svr.log_lines(new_log, r"vhmd: \[(warn|error)\]")
            sample = {
                "t_min": round((now - start) / 60, 2),
                "scene_fps": round(frames / dt, 1),
                "presents_per_s": round((presents - last_presents) / dt, 1) if presents is not None and last_presents is not None else None,
                "rejected_new": rejected - (last_rejected or 0),
                "max_device_age_s": round(max(ages), 3) if ages else None,
                "pose_error_cm": round(worst_err * 100, 2),
                "vrserver_mb": vrserver_memory_mb(),
                "vhmd_warnings": len(vhmd_bad),
                "steamvr_running": bool(svr.running(("vrserver",))),
            }
            samples.append(sample)
            for line in vhmd_bad[:3]:
                problems.append(line.strip()[:200])
            print(json.dumps(sample), flush=True)
            frames, worst_err, last_presents, last_rejected, last_t = 0, 0.0, presents, rejected, now
            next_sample += args.interval

    sender.stop.set()
    sender.join(2)
    openvr.shutdown()

    # Verdicts. The first sample includes start-up, so memory growth is measured from the second.
    def check(name, ok, detail):
        return {"name": name, "ok": bool(ok), "detail": detail}

    fps = [s["scene_fps"] for s in samples]
    pres = [s["presents_per_s"] for s in samples if s["presents_per_s"] is not None]
    mem = [s["vrserver_mb"] for s in samples if s["vrserver_mb"] is not None]
    base = mem[1] if len(mem) > 1 else (mem[0] if mem else None)
    results = [
        check("samples taken", len(samples) >= max(1, int(args.minutes * 60 / args.interval) - 1), f"{len(samples)}"),
        check("SteamVR stayed up", all(s["steamvr_running"] for s in samples), ""),
        check(f"Scene app frame rate >= {args.min_fps:g}", fps and min(fps) >= args.min_fps,
              f"min {min(fps) if fps else 'n/a'}, avg {round(sum(fps) / len(fps), 1) if fps else 'n/a'}"),
        check("no rejected packets", sum(s["rejected_new"] for s in samples) == 0,
              f"{sum(s['rejected_new'] for s in samples)}"),
        check("devices kept updating (age < 0.5 s)", all((s["max_device_age_s"] or 0) < 0.5 for s in samples),
              f"max {max((s['max_device_age_s'] or 0) for s in samples) if samples else 'n/a'} s"),
        check(f"right controller pose error < {args.max_pose_error_cm:g} cm",
              all(s["pose_error_cm"] < args.max_pose_error_cm for s in samples),
              f"max {max((s['pose_error_cm'] for s in samples), default=0)} cm"),
        check(f"vrserver memory growth < {args.max_memory_growth_mb:g} MB",
              base is not None and mem[-1] - base < args.max_memory_growth_mb,
              f"{base} -> {mem[-1] if mem else None} MB"),
        check("no vhmd warnings/errors in vrserver.txt", sum(s["vhmd_warnings"] for s in samples) == 0,
              f"{sum(s['vhmd_warnings'] for s in samples)}" + (f"; first: {problems[0]}" if problems else "")),
    ]
    if pres:
        results.insert(3, check(f"presents/s >= {args.min_fps:g} (direct mode)", min(pres) >= args.min_fps,
                                f"min {min(pres)}, avg {round(sum(pres) / len(pres), 1)}"))

    if started_here:
        try:
            svr.stop_steamvr(timeout=60)
        except (TimeoutError, RuntimeError) as e:
            print(f"SteamVR stop: {e}")

    out_dir = HERE / "results"
    out_dir.mkdir(exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    report = {"minutes": args.minutes, "platform": sys.platform, "display_mode": mode, "min_fps": args.min_fps, "results": results, "samples": samples,
              "packets_sent_by_test": sender.sent}
    (out_dir / f"soak_{stamp}.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    ok = all(r["ok"] for r in results)
    md = [f"# vhmd soak test — {stamp}", "", f"**{'PASSED' if ok else 'FAILED'}** — {args.minutes:g} min, {sys.platform}, display mode {mode}", "",
          "| | Check | Detail |", "|---|---|---|"]
    md += [f"| {'PASS' if r['ok'] else 'FAIL'} | {r['name']} | {r['detail']} |" for r in results]
    md += ["", "| t (min) | Scene fps | presents/s | pose err (cm) | vrserver MB | max age (s) |", "|---|---|---|---|---|---|"]
    md += [f"| {s['t_min']} | {s['scene_fps']} | {s['presents_per_s']} | {s['pose_error_cm']} | {s['vrserver_mb']} | {s['max_device_age_s']} |"
           for s in samples]
    (out_dir / f"soak_{stamp}.md").write_text("\n".join(md) + "\n", encoding="utf-8")
    for r in results:
        print(f"[{'PASS' if r['ok'] else 'FAIL'}] {r['name']} -- {r['detail']}")
    print(f"{'ALL PASSED' if ok else 'FAILURES'}  report: {out_dir / f'soak_{stamp}.md'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
