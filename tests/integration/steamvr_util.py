"""Helpers to start/stop SteamVR and read its logs (Windows and Linux).

The Windows code paths were verified against SteamVR 2.17.10. The Linux code
paths (process handling via /proc and signals, vrstartup.sh, ~/.steam paths)
have not been run against a real Linux SteamVR yet.
"""

from __future__ import annotations

import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Optional

IS_WINDOWS = sys.platform == "win32"
if IS_WINDOWS:
    import winreg


def _openvrpaths_file() -> Path:
    if IS_WINDOWS:
        return Path(os.environ["LOCALAPPDATA"]) / "openvr" / "openvrpaths.vrpath"
    return Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / "openvr" / "openvrpaths.vrpath"


def _openvrpaths() -> dict:
    try:
        return json.loads(_openvrpaths_file().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def steam_path() -> Path:
    if IS_WINDOWS:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            return Path(winreg.QueryValueEx(k, "SteamPath")[0])
    for p in (Path.home() / ".steam" / "steam", Path.home() / ".local" / "share" / "Steam",
              Path.home() / ".var" / "app" / "com.valvesoftware.Steam" / ".local" / "share" / "Steam"):
        if (p / "steamapps").exists():
            return p.resolve()
    return Path.home() / ".steam" / "steam"


def steamvr_path() -> Path:
    marker = Path("bin") / ("win64/vrserver.exe" if IS_WINDOWS else "linux64/vrserver")
    for r in _openvrpaths().get("runtime", []):
        if (Path(r) / marker).exists():
            return Path(r)
    return steam_path() / "steamapps" / "common" / "SteamVR"


def vrcmd_path() -> Path:
    return steamvr_path() / "bin" / ("win64/vrcmd.exe" if IS_WINDOWS else "linux64/vrcmd")


def vrserver_log() -> Path:
    logs = _openvrpaths().get("log") or []
    if logs:
        return Path(logs[0]) / "vrserver.txt"
    return steam_path() / "logs" / "vrserver.txt"


def _norm(name: str) -> str:
    name = name.lower()
    return name[:-4] if name.endswith(".exe") else name


def _process_table() -> List[tuple]:
    """[(pid, name)] for all processes; names lower-case without .exe."""
    if IS_WINDOWS:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
        rows = []
        for line in out.splitlines():
            parts = [p.strip('"') for p in line.split('","')]
            if len(parts) >= 2 and parts[1].strip('"').isdigit():
                rows.append((int(parts[1].strip('"')), _norm(parts[0])))
        return rows
    rows = []
    for d in Path("/proc").iterdir():
        if d.name.isdigit():
            try:
                rows.append((int(d.name), _norm((d / "comm").read_text().strip())))
            except OSError:
                pass
    return rows


def running(names=("vrserver", "vrmonitor", "vrcompositor")) -> List[str]:
    """Names (as given) of the listed processes that are running. Accepts
    names with or without '.exe'."""
    present = {n for _, n in _process_table()}
    return [n for n in names if _norm(n) in present]


def start_steamvr() -> None:
    if IS_WINDOWS:
        exe = steamvr_path() / "bin" / "win64" / "vrstartup.exe"
        subprocess.Popen([str(exe)], cwd=str(exe.parent), close_fds=True)
        return
    script = steamvr_path() / "bin" / "vrstartup.sh"
    if script.exists():
        subprocess.Popen([str(script)], cwd=str(script.parent), start_new_session=True,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    else:  # let the Steam client launch it
        subprocess.Popen(["xdg-open", "steam://rungameid/250820"], start_new_session=True)


def wait_for_server(openvr_module, timeout: float = 90.0):
    """Polls until a background OpenVR client can connect. Returns the IVRSystem."""
    deadline = time.monotonic() + timeout
    last_err = None
    while time.monotonic() < deadline:
        try:
            return openvr_module.init(openvr_module.VRApplication_Background)
        except openvr_module.OpenVRError as e:  # server not up yet
            last_err = e
            time.sleep(1.0)
    raise TimeoutError(f"SteamVR did not come up within {timeout}s: {last_err}")


_STEAMVR_PROCS = {"vrserver", "vrmonitor", "vrcompositor", "vrdashboard", "vrwebhelper", "vrstartup"}


def _is_steam_or_game(name: str, path: str) -> bool:
    """Steam client processes and games are never ours to stop."""
    p = path.lower().replace("\\", "/")
    return name.startswith("steam") or "/steam/" in p or "/steamapps/" in p or "/.steam/" in p


def openvr_client_processes() -> List[tuple]:
    """[(pid, name, path)] of running processes that loaded the OpenVR client
    library (vrclient_x64.dll / vrclient.so): OpenVR apps and overlays.
    Excludes SteamVR itself, the Steam client, games under steamapps (this
    includes Proton games) and this process."""
    rows = []
    if IS_WINDOWS:
        ps = (
            "Get-Process | Where-Object { try { $_.Modules.ModuleName -contains 'vrclient_x64.dll' } catch { $false } }"
            " | ForEach-Object { \"$($_.Id)|$($_.ProcessName)|$($_.Path)\" }"
        )
        out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
        candidates = []
        for line in out.splitlines():
            parts = line.strip().split("|")
            if len(parts) == 3 and parts[0].isdigit():
                candidates.append((int(parts[0]), parts[1], parts[2]))
    else:
        candidates = []
        for d in Path("/proc").iterdir():
            if not d.name.isdigit():
                continue
            try:
                if "vrclient" not in (d / "maps").read_text(errors="replace"):
                    continue
                candidates.append((int(d.name), (d / "comm").read_text().strip(), os.readlink(d / "exe")))
            except OSError:
                continue
    for pid, name, path in candidates:
        if pid == os.getpid() or _norm(name) in _STEAMVR_PROCS or _is_steam_or_game(name.lower(), path):
            continue
        rows.append((pid, name, path))
    return rows


def _kill_pid(pid: int) -> None:
    if IS_WINDOWS:
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    else:
        try:
            os.kill(pid, signal.SIGTERM)
        except OSError:
            pass


def _kill_name(name: str) -> None:
    if IS_WINDOWS:
        subprocess.run(["taskkill", "/F", "/IM", f"{name}.exe"], capture_output=True)
    else:
        for pid, n in _process_table():
            if n == name:
                _kill_pid(pid)


def stop_steamvr(timeout: float = 60.0, stop_relaunchers: bool = False, relaunch_watch: float = 8.0) -> float:
    """Shuts SteamVR down and waits until it stays down. Returns seconds taken.

    Windows: vrmonitor ignores WM_CLOSE from taskkill; when the master process
    goes away vrserver logs "Lost master process ... Quitting all immediately"
    and exits through its normal path (drivers get Cleanup()). On Linux the
    same is attempted with SIGTERM to vrmonitor (unverified).

    Some OpenVR overlay apps that SteamVR auto-starts survive that and call
    VR_Init again right away, which relaunches SteamVR. Without
    stop_relaunchers such a relaunch raises RuntimeError naming the OpenVR
    client processes still running; with it, those processes are terminated
    (SteamVR auto-starts them again with its next session) and SteamVR is
    stopped again.
    """
    t0 = time.monotonic()
    for _ in range(3):
        clients = openvr_client_processes()
        if stop_relaunchers:
            # Before SteamVR goes down, so they cannot relaunch it.
            for pid, name, path in clients:
                print(f"stop_steamvr: terminating OpenVR overlay/app: {name} (pid {pid}, {path})")
                _kill_pid(pid)
        _kill_name("vrmonitor")
        while running(("vrserver",)):
            if time.monotonic() - t0 > timeout:
                raise TimeoutError("vrserver did not exit")
            time.sleep(0.25)
        took = time.monotonic() - t0
        # Make sure nothing brings SteamVR straight back.
        deadline = time.monotonic() + relaunch_watch
        relaunched = False
        while time.monotonic() < deadline:
            if running(("vrmonitor", "vrserver", "vrstartup")):
                relaunched = True
                break
            time.sleep(0.5)
        if not relaunched:
            return took
        if not stop_relaunchers:
            names = ", ".join(f"{n} (pid {p})" for p, n, _ in clients) or "unknown process"
            raise RuntimeError(f"SteamVR was relaunched after shutdown; OpenVR clients still running: {names}")
    raise RuntimeError("SteamVR keeps relaunching")


def read_log_since(path: Path, offset: int) -> str:
    try:
        with open(path, "rb") as f:
            f.seek(offset)
            return f.read().decode("utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def latest_session(text: str) -> str:
    """vrserver.txt can hold several sessions; return the text of the last one."""
    idx = text.rfind("startup with PID=")
    if idx < 0:
        return text
    return text[text.rfind("\n", 0, idx) + 1:]


def log_lines(text: str, pattern: str) -> List[str]:
    rx = re.compile(pattern)
    return [l for l in text.splitlines() if rx.search(l)]


def log_size(path: Optional[Path] = None) -> int:
    p = path or vrserver_log()
    return p.stat().st_size if p.exists() else 0
