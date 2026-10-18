"""Helpers to start/stop SteamVR and read its logs (Windows)."""

from __future__ import annotations

import json
import os
import re
import subprocess
import time
import winreg
from pathlib import Path
from typing import List, Optional


def steam_path() -> Path:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
        return Path(winreg.QueryValueEx(k, "SteamPath")[0])


def steamvr_path() -> Path:
    vrpath = Path(os.environ["LOCALAPPDATA"]) / "openvr" / "openvrpaths.vrpath"
    data = json.loads(vrpath.read_text(encoding="utf-8"))
    for r in data.get("runtime", []):
        if (Path(r) / "bin" / "win64" / "vrserver.exe").exists():
            return Path(r)
    return steam_path() / "steamapps" / "common" / "SteamVR"


def vrserver_log() -> Path:
    return steam_path() / "logs" / "vrserver.txt"


def running(names=("vrserver.exe", "vrmonitor.exe", "vrcompositor.exe")) -> List[str]:
    out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True).stdout
    found = []
    for line in out.splitlines():
        name = line.split(",")[0].strip('"').lower()
        if name in names:
            found.append(name)
    return found


def start_steamvr() -> None:
    exe = steamvr_path() / "bin" / "win64" / "vrstartup.exe"
    subprocess.Popen([str(exe)], cwd=str(exe.parent), close_fds=True)


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


def openvr_client_processes() -> List[tuple]:
    """[(pid, name, path)] of running processes that loaded vrclient_x64.dll
    (OpenVR apps / overlays), excluding SteamVR itself, the Steam client and
    this process. Steam (steam.exe, steamwebhelper.exe, ...) loads vrclient
    too and must never be touched."""
    ps = (
        "Get-Process | Where-Object { try { $_.Modules.ModuleName -contains 'vrclient_x64.dll' } catch { $false } }"
        " | ForEach-Object { \"$($_.Id)|$($_.ProcessName)|$($_.Path)\" }"
    )
    out = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True).stdout
    rows = []
    for line in out.splitlines():
        parts = line.strip().split("|")
        if len(parts) == 3 and parts[0].isdigit():
            pid = int(parts[0])
            name, path = parts[1].lower(), parts[2].lower()
            if pid == os.getpid() or name in _STEAMVR_PROCS:
                continue
            if name.startswith("steam") or "\\steam\\" in path or "\\steamapps\\" in path:
                continue  # Steam client / games: never ours to stop
            rows.append((pid, parts[1], parts[2]))
    return rows


def stop_steamvr(timeout: float = 60.0, stop_relaunchers: bool = False, relaunch_watch: float = 8.0) -> float:
    """Shuts SteamVR down and waits until it stays down. Returns seconds taken.

    vrmonitor ignores WM_CLOSE from taskkill; when the master process goes away
    vrserver logs "Lost master process ... Quitting all immediately" and exits
    through its normal path (drivers get Cleanup()).

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
                subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
        subprocess.run(["taskkill", "/F", "/IM", "vrmonitor.exe"], capture_output=True)
        while "vrserver.exe" in running():
            if time.monotonic() - t0 > timeout:
                raise TimeoutError("vrserver did not exit")
            time.sleep(0.25)
        took = time.monotonic() - t0
        # Make sure nothing brings SteamVR straight back.
        deadline = time.monotonic() + relaunch_watch
        relaunched = False
        while time.monotonic() < deadline:
            if running(("vrmonitor.exe", "vrserver.exe", "vrstartup.exe")):
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
