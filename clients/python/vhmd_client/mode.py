"""Switch SteamVR between the VHMD headset and a physical one (e.g. Pico 4 Ultra
through PICO Connect). Windows only.

    python -m vhmd_client.mode            # show the current mode
    python -m vhmd_client.mode pico       # physical HMD + VHMD trackers
    python -m vhmd_client.mode vhmd       # VHMD HMD and controllers, physical driver off

SteamVR picks its HMD when it starts and cannot change it while running, so a
switch restarts SteamVR (about 20-30 s; running VR apps close). Nothing else is
touched: the mode is two SteamVR settings,

- ``driver_<physical>.enable``  (the physical headset's driver on / off), and
- ``driver_vhmd.configFile``    (a VHMD config derived from the installed one,
  with ``hmd`` / ``controllers`` enabled or not),

written through SteamVR's own settings API while it runs (so SteamVR does not
overwrite them) and into steamvr.vrsettings while it is stopped. The derived
configs live in %LOCALAPPDATA%/VHMD/profiles and are rebuilt from the installed
config on every switch, so edits to that config carry over.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path
from typing import Callable, Dict, List, Optional

if sys.platform != "win32":
    raise SystemExit("vhmd_client.mode is Windows only")

import winreg

MODES = ("vhmd", "pico")
PHYSICAL_DRIVER = "pico"              # driver name in its driver.vrdrivermanifest
DRIVER_SECTION = "driver_vhmd"
PROFILE_DIR = Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) / "VHMD" / "profiles"
# What each mode enables in the VHMD config (trackers keep their own settings).
PROFILE_FLAGS = {"vhmd": {"hmd": True, "controllers": True}, "pico": {"hmd": False, "controllers": False}}

Progress = Callable[[str], None]


# -- locations ----------------------------------------------------------------
def _openvrpaths() -> dict:
    p = Path(os.environ["LOCALAPPDATA"]) / "openvr" / "openvrpaths.vrpath"
    try:
        return json.loads(p.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def steam_path() -> Path:
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
        return Path(winreg.QueryValueEx(k, "SteamPath")[0])


def settings_file() -> Path:
    for c in _openvrpaths().get("config", []):
        p = Path(c) / "steamvr.vrsettings"
        if p.exists():
            return p
    return steam_path() / "config" / "steamvr.vrsettings"


def steamvr_dir() -> Path:
    for r in _openvrpaths().get("runtime", []):
        if (Path(r) / "bin" / "win64" / "vrserver.exe").exists():
            return Path(r)
    return steam_path() / "steamapps" / "common" / "SteamVR"


def driver_dir(name: str) -> Optional[Path]:
    """Registered driver folder whose manifest has this name."""
    for d in _openvrpaths().get("external_drivers", []):
        m = Path(d) / "driver.vrdrivermanifest"
        try:
            if json.loads(m.read_text(encoding="utf-8")).get("name") == name:
                return Path(d)
        except (OSError, ValueError):
            continue
    return None


def base_config() -> Path:
    d = driver_dir("vhmd")
    if d is None:
        raise SystemExit("the vhmd driver is not registered (tools/install_driver.ps1)")
    return d / "resources" / "settings" / "vhmd_config.json"


# -- profiles -----------------------------------------------------------------
def _set_enabled(text: str, section: str, value: bool) -> str:
    """Sets "enabled" inside a top-level section of the (commented) JSON config."""
    m = re.search(r'"%s"\s*:\s*\{' % re.escape(section), text)
    if not m:
        raise ValueError(f"section {section!r} not found in the VHMD config")
    e = re.compile(r'"enabled"\s*:\s*(true|false)').search(text, m.end())
    if not e:
        raise ValueError(f"{section}.enabled not found in the VHMD config")
    return text[:e.start()] + f'"enabled": {"true" if value else "false"}' + text[e.end():]


def write_profile(mode: str) -> Path:
    text = base_config().read_text(encoding="utf-8")
    for section, value in PROFILE_FLAGS[mode].items():
        text = _set_enabled(text, section, value)
    PROFILE_DIR.mkdir(parents=True, exist_ok=True)
    out = PROFILE_DIR / f"{mode}.json"
    out.write_text(text, encoding="utf-8")
    return out


def wanted_settings(mode: str) -> Dict[str, Dict[str, object]]:
    return {f"driver_{PHYSICAL_DRIVER}": {"enable": mode == "pico"},
            DRIVER_SECTION: {"configFile": str(write_profile(mode))}}


# -- SteamVR processes --------------------------------------------------------
def running(image: str) -> bool:
    out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {image}", "/NH"], capture_output=True, text=True,
                         creationflags=subprocess.CREATE_NO_WINDOW).stdout
    return image.lower() in out.lower()


def steamvr_running() -> bool:
    return running("vrserver.exe")


def stop_steamvr(progress: Progress, timeout: float = 40.0) -> None:
    """Closes SteamVR the way its own window does: without vrmonitor, vrserver
    shuts down gracefully ("Lost master process"). Only SteamVR's processes."""
    t0 = time.time()
    while steamvr_running() and time.time() - t0 < timeout:
        if running("vrmonitor.exe"):
            subprocess.run(["taskkill", "/F", "/IM", "vrmonitor.exe"], capture_output=True,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        time.sleep(1.0)
    if steamvr_running():
        # Started by another program (e.g. PICO Connect) without a vrmonitor
        # that owns it: end the SteamVR processes directly.
        progress("SteamVR did not close; ending its processes")
        for image in ("vrserver.exe", "vrcompositor.exe", "vrdashboard.exe", "vrwebhelper.exe"):
            subprocess.run(["taskkill", "/F", "/IM", image], capture_output=True,
                           creationflags=subprocess.CREATE_NO_WINDOW)
        time.sleep(2.0)


def start_steamvr() -> None:
    exe = steamvr_dir() / "bin" / "win64" / "vrstartup.exe"
    subprocess.Popen([str(exe)], cwd=str(exe.parent), close_fds=True,
                     creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)


# -- settings -----------------------------------------------------------------
def _apply_via_api(settings: Dict[str, Dict[str, object]]) -> bool:
    """Writes the settings through a running SteamVR (needs pyopenvr)."""
    try:
        import openvr
    except ImportError:
        return False
    try:
        openvr.init(openvr.VRApplication_Utility)
    except Exception:
        return False
    try:
        s = openvr.VRSettings()
        for section, values in settings.items():
            for k, v in values.items():
                if isinstance(v, bool):
                    s.setBool(section, k, v)
                else:
                    s.setString(section, k, str(v))
        s.sync(True)
        return True
    except Exception:
        return False
    finally:
        openvr.shutdown()


def _apply_to_file(settings: Dict[str, Dict[str, object]]) -> None:
    """Edits steamvr.vrsettings (only while SteamVR is stopped)."""
    p = settings_file()
    data = json.loads(p.read_text(encoding="utf-8")) if p.exists() else {}
    for section, values in settings.items():
        data.setdefault(section, {}).update(values)
    # A crash during a switch can leave the driver disabled by SteamVR's safe mode.
    data.get(DRIVER_SECTION, {}).pop("blocked_by_safe_mode", None)
    p.write_text(json.dumps(data, indent=3, ensure_ascii=False), encoding="utf-8")


def current_mode() -> Optional[str]:
    """The mode the settings select (None if they match neither)."""
    try:
        data = json.loads(settings_file().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    physical_on = data.get(f"driver_{PHYSICAL_DRIVER}", {}).get("enable", True)
    cfg = data.get(DRIVER_SECTION, {}).get("configFile", "")
    if cfg:
        name = Path(cfg).stem
        if name in MODES and (name == "pico") == bool(physical_on):
            return name
        return None
    return "pico" if physical_on else None


def switch(mode: str, progress: Progress = print, restart: bool = True) -> None:
    """Selects a mode and (re)starts SteamVR with it."""
    if mode not in MODES:
        raise ValueError(f"mode must be one of {MODES}")
    if driver_dir(PHYSICAL_DRIVER) is None and mode == "pico":
        raise SystemExit(f"the {PHYSICAL_DRIVER} driver is not registered")
    settings = wanted_settings(mode)
    was_running = steamvr_running()
    if was_running:
        progress("saving the settings through SteamVR")
        _apply_via_api(settings)
        progress("closing SteamVR")
        stop_steamvr(progress)
    _apply_to_file(settings)
    if restart or was_running:
        progress("starting SteamVR")
        start_steamvr()
        t0 = time.time()
        while not steamvr_running() and time.time() - t0 < 60:
            time.sleep(0.5)
        if mode == "vhmd":
            _wait_driver(progress)
    progress(f"mode: {mode}")


def _wait_driver(progress: Progress, timeout: float = 60.0) -> None:
    from .client import VHMDClient
    c = VHMDClient(timeout=1.0)
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            if c.ping():
                return
        except Exception:
            pass
        time.sleep(1.0)
    progress("the VHMD driver did not answer yet")


def main() -> None:
    ap = argparse.ArgumentParser(description="Switch SteamVR between the VHMD headset and a physical one")
    ap.add_argument("mode", nargs="?", choices=MODES, help="omit to show the current mode")
    ap.add_argument("--no-start", action="store_true", help="when SteamVR is not running, do not start it")
    args = ap.parse_args()
    if args.mode is None:
        print("mode:", current_mode() or "custom", "| SteamVR running:", steamvr_running())
        return
    switch(args.mode, restart=not args.no_start)


if __name__ == "__main__":
    main()
