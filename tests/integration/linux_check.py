"""Linux SteamVR verification for the vhmd direct-mode HMD (v0.3.0).

Run on a Linux PC with SteamVR installed and the driver built + registered
(tools/install_driver.sh). It starts SteamVR, checks that the compositor
runs and hands frames to the driver, grabs a preview snapshot, runs the
regular integration test, stops SteamVR and writes a report to send back:

    python3 tests/integration/linux_check.py
    python3 tests/integration/linux_check.py --skip-integration   # faster, display path only

Output: tests/integration/results/linux_check_<time>/ with report.md,
report.json, eyes.png (if frames arrived) and the relevant log excerpts.
Nothing outside SteamVR is changed; SteamVR is stopped at the end only if
this script started it.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / "clients" / "python"))
sys.path.insert(0, str(HERE))

import steamvr_util as svr  # noqa: E402
from vhmd_client import VHMDClient  # noqa: E402
from vhmd_client.preview import DEFAULT_PATH, PreviewReader  # noqa: E402


class Report:
    def __init__(self, out_dir: Path):
        self.out_dir = out_dir
        self.items = []
        self.info = {}

    def add(self, name: str, status: str, detail: str = "") -> None:
        """status: PASS / FAIL / WARN / INFO / SKIP"""
        self.items.append({"name": name, "status": status, "detail": detail})
        print(f"[{status:4}] {name}" + (f" -- {detail}" if detail else ""), flush=True)

    def check(self, name: str, ok: bool, detail: str = "") -> bool:
        self.add(name, "PASS" if ok else "FAIL", detail)
        return ok

    def write(self) -> Path:
        (self.out_dir / "report.json").write_text(
            json.dumps({"items": self.items, "info": self.info}, indent=2, default=str), encoding="utf-8")
        lines = [f"# vhmd Linux check — {datetime.now():%Y-%m-%d %H:%M}", ""]
        fails = [i for i in self.items if i["status"] == "FAIL"]
        lines += [f"**Result: {'FAILED' if fails else 'PASSED'}** "
                  f"({sum(i['status'] == 'PASS' for i in self.items)} pass, {len(fails)} fail)", ""]
        lines += ["| | Check | Detail |", "|---|---|---|"]
        for i in self.items:
            lines.append(f"| {i['status']} | {i['name']} | {str(i['detail']).replace('|', '/')[:300]} |")
        lines += ["", "## Environment", "", "```", json.dumps(self.info.get("env", {}), indent=2), "```", ""]
        if "status" in self.info:
            lines += ["## Driver status (config list)", "", "```",
                      json.dumps(self.info["status"].get("display", {}), indent=2), "```", ""]
        lines += ["Log excerpts are in this folder (vrserver_vhmd.txt, vrcompositor_tail.txt)."]
        md = self.out_dir / "report.md"
        md.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return md


def sh(cmd: str, timeout: float = 10) -> str:
    try:
        return subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout).stdout.strip()
    except Exception as e:  # noqa: BLE001
        return f"<{e}>"


def environment(rep: Report) -> None:
    env = {
        "distro": sh(". /etc/os-release && echo $PRETTY_NAME"),
        "kernel": platform.release(),
        "session": os.environ.get("XDG_SESSION_TYPE", "?"),
        "desktop": os.environ.get("XDG_CURRENT_DESKTOP", "?"),
        "wayland_display": os.environ.get("WAYLAND_DISPLAY", ""),
        "x_display": os.environ.get("DISPLAY", ""),
        "glibc": platform.libc_ver()[1],
        "gpu": sh("lspci 2>/dev/null | grep -iE 'vga|3d|display'"),
        "vulkan_devices": sh("vulkaninfo --summary 2>/dev/null | grep -E 'deviceName|driverName'") or "vulkaninfo not installed",
        "steamvr_path": str(svr.steamvr_path()),
        "steamvr_version_txt": sh(f"head -1 '{svr.steamvr_path()}/bin/version.txt'"),
        "python": sys.version.split()[0],
    }
    for acf in (svr.steam_path() / "steamapps" / "appmanifest_250820.acf",):
        if acf.exists():
            text = acf.read_text(errors="replace")
            for key in ("buildid", "BetaKey", "TargetBuildID"):
                if f'"{key}"' in text:
                    env[f"steamvr_{key}"] = text.split(f'"{key}"')[1].split('"')[1]
    rep.info["env"] = env
    rep.add("environment", "INFO", f"{env['distro']}, {env['session']} ({env['desktop']}), SteamVR build "
                                   f"{env.get('steamvr_buildid', '?')} branch {env.get('steamvr_BetaKey', 'public')}")

    so = ROOT / "build" / "driver" / "vhmd" / "bin" / "linux64" / "driver_vhmd.so"
    packaged = ROOT / "vhmd" / "bin" / "linux64" / "driver_vhmd.so"
    so = packaged if packaged.exists() else so
    rep.check("driver built", so.exists(), str(so))
    vrpaths = svr._openvrpaths().get("external_drivers") or []
    rep.check("driver registered with SteamVR", any(Path(p, "bin/linux64/driver_vhmd.so").exists() for p in vrpaths),
              ", ".join(vrpaths) or "no external drivers (run tools/install_driver.sh)")


def wait_display(client: VHMDClient, timeout: float) -> dict:
    """Polls `config list` until the compositor presents frames or timeout."""
    t0 = time.time()
    st = {}
    while time.time() - t0 < timeout:
        try:
            st = client.status()
        except (TimeoutError, OSError):
            time.sleep(0.5)
            continue
        if st.get("display", {}).get("presents", 0) > 30:
            break
        time.sleep(0.5)
    return st


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=39570)
    ap.add_argument("--skip-integration", action="store_true", help="skip tests/integration/run_integration.py")
    ap.add_argument("--keep-running", action="store_true", help="leave SteamVR running at the end")
    ap.add_argument("--preview-path", default=DEFAULT_PATH)
    ap.add_argument("--ping-timeout", type=float, default=120.0, help="seconds to wait for SteamVR to load the driver")
    ap.add_argument("--hold", type=float, default=60.0,
                    help="seconds to keep SteamVR running before the final present check (catches late compositor failures)")
    args = ap.parse_args()

    out = HERE / "results" / f"linux_check_{datetime.now():%Y%m%d_%H%M%S}"
    out.mkdir(parents=True, exist_ok=True)
    rep = Report(out)
    if sys.platform != "linux":
        rep.add("platform", "FAIL", "run this on Linux")
        rep.write()
        return 2

    environment(rep)

    started_here = False
    if svr.running(("vrserver",)):
        rep.add("SteamVR already running", "WARN", "using the running session (restart it to load a new driver build)")
    else:
        try:
            svr.start_steamvr()
            started_here = True
        except OSError as e:
            rep.add("start SteamVR", "FAIL", str(e))

    client = VHMDClient(port=args.port, timeout=1.0)
    t0 = time.time()
    while not client.ping():
        if time.time() - t0 > args.ping_timeout:
            break
    up = client.ping()
    rep.check("driver answers on UDP (vrserver loaded vhmd)", up, f"after {time.time() - t0:.1f} s")

    st = wait_display(client, 45) if up else {}
    rep.info["status"] = st
    disp = st.get("display", {})
    rep.check("HMD uses direct mode", disp.get("mode") == "direct", f"display.mode = {disp.get('mode')}")
    rep.check("vsync clock running", disp.get("vsyncs", 0) > 30, f"vsyncs = {disp.get('vsyncs')}")
    rep.check("compositor created swap texture sets", disp.get("swap_sets_created", 0) > 0,
              f"{disp.get('swap_sets_created')} sets, eye texture {disp.get('eye_texture')}")
    p1 = disp.get("presents", 0)
    time.sleep(2.0)
    try:
        p2 = client.status().get("display", {}).get("presents", 0)
    except (TimeoutError, OSError):
        p2 = p1
    rate = (p2 - p1) / 2.0
    rep.check("compositor presents frames to the driver", rate > 30, f"{rate:.0f} presents/s (presents {p1} -> {p2})")
    rep.check("vrcompositor process alive", bool(svr.running(("vrcompositor",))))

    # Logs
    log_dir = svr.vrserver_log().parent
    vrs = svr.latest_session(svr.read_log_since(svr.vrserver_log(), 0))
    (out / "vrserver_vhmd.txt").write_text(
        "\n".join(svr.log_lines(vrs, r"vhmd|Active HMD|direct mode|DirectMode|Compositor|compositor")[-400:]),
        encoding="utf-8")
    comp_log = log_dir / "vrcompositor.txt"
    comp = svr.latest_session(svr.read_log_since(comp_log, 0)) if comp_log.exists() else ""
    (out / "vrcompositor_tail.txt").write_text("\n".join(comp.splitlines()[-300:]), encoding="utf-8")
    drm = svr.log_lines(comp, r"CannotDRMLeaseDisplay|VR requires direct mode|Failed to start compositor|Failed to initialize compositor")
    if comp:
        rep.check("no DRM-lease / direct-mode errors in vrcompositor.txt", not drm, drm[0][:200] if drm else "")
    else:
        rep.add("no DRM-lease / direct-mode errors in vrcompositor.txt", "SKIP", f"{comp_log} not found")
    rep.check("vrserver.txt: driver chose direct mode", bool(svr.log_lines(vrs, r"vhmd: HMD display: direct mode")))
    rep.check("vrserver.txt: first frame presented", bool(svr.log_lines(vrs, r"vhmd: direct mode: first frame presented")))
    errors = svr.log_lines(vrs, r"vhmd: \[(error|warn)\]")
    rep.add("vhmd warnings/errors in vrserver.txt", "INFO" if not errors else "WARN",
            f"{len(errors)} lines" + (f", first: {errors[0][:160]}" if errors else ""))

    # Preview
    pv = disp.get("preview", {})
    rep.add("preview state (driver)", "INFO", f"{pv}")
    frame = None
    with PreviewReader(args.preview_path) as reader:
        deadline = time.time() + 10
        first = None
        while time.time() < deadline:
            f = reader.read()
            if f and first is None:
                first = f.frame_index
            if f and first is not None and f.frame_index > first + 2:
                frame = f
                break
            time.sleep(0.2)
    if frame:
        png = out / "eyes.png"
        frame.save_png(str(png))
        r, g, b = frame.mean_rgb()
        rep.check("preview frames in shared memory", True,
                  f"{frame.width}x{frame.height}, frame #{frame.frame_index}, age {frame.age_s * 1000:.0f} ms, "
                  f"mean RGB ({r:.0f},{g:.0f},{b:.0f}) -> {png.name}")
    else:
        rep.check("preview frames in shared memory", False, f"no advancing frames at {args.preview_path}")

    # Regular integration test (enumeration, poses, inputs, skeleton, load)
    if args.skip_integration or not up:
        rep.add("integration test", "SKIP")
    else:
        proc = subprocess.run([sys.executable, str(HERE / "run_integration.py"), "--no-start", "--keep-running",
                               "--port", str(args.port)], capture_output=True, text=True, timeout=600)
        (out / "integration_output.txt").write_text(proc.stdout + proc.stderr, encoding="utf-8")
        summary = [l for l in proc.stdout.splitlines() if l.startswith(("ALL PASSED", "FAILURES"))]
        fails = [l for l in proc.stdout.splitlines() if l.startswith("[FAIL]")]
        rep.check("integration test (run_integration.py)", proc.returncode == 0,
                  (summary[0] if summary else "no summary") + ("; " + " | ".join(f[:120] for f in fails[:5]) if fails else ""))

    # The compositor must still be presenting at the end: some GPUs fail a while after start-up.
    if up:
        t_end = time.monotonic() + args.hold
        while time.monotonic() < t_end:
            time.sleep(1.0)
        try:
            q1 = client.status().get("display", {}).get("presents", 0)
            time.sleep(2.0)
            q2 = client.status().get("display", {}).get("presents", 0)
        except (TimeoutError, OSError):
            q1 = q2 = 0
        late_rate = (q2 - q1) / 2.0
        comp_now = svr.latest_session(svr.read_log_since(comp_log, 0)) if comp_log.exists() else ""
        lost = svr.log_lines(comp_now, r"vkerror=-4|VK_ERROR_DEVICE_LOST|Watchdog timeout")
        rep.check("compositor still presenting at the end", late_rate > 30 and not lost,
                  f"{late_rate:.0f} presents/s after {time.time() - t0:.0f} s"
                  + (f"; {lost[0].strip()[:160]}" if lost else ""))

    client.close()
    if started_here and not args.keep_running:
        try:
            took = svr.stop_steamvr(timeout=60)
            vrs = svr.latest_session(svr.read_log_since(svr.vrserver_log(), 0))
            rep.check("SteamVR shutdown: driver Cleanup ran", bool(svr.log_lines(vrs, r"vhmd: shutting down")),
                      f"{took:.1f} s")
        except svr.ForcedShutdown as e:
            rep.add("SteamVR shutdown", "SKIP", str(e))
        except (TimeoutError, RuntimeError) as e:
            rep.add("SteamVR shutdown", "WARN", str(e))

    md = rep.write()
    print(f"\nReport: {md}")
    print(f"Send back the folder: {out}")
    return 0 if not any(i["status"] == "FAIL" for i in rep.items) else 1


if __name__ == "__main__":
    sys.exit(main())
