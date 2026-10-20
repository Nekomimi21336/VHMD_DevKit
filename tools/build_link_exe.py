"""Builds "VHMD Link.exe", the PC app as a standalone program (no Python
needed on the PC that runs it), with PyInstaller.

    python -m pip install pyinstaller pillow      # once
    python tools/build_link_exe.py                # -> dist/VHMD Link/VHMD Link.exe (folder)
    python tools/build_link_exe.py --onefile      # -> dist/VHMD Link.exe (one file, starts slower)

The exe also runs the keyboard/mouse tool (`"VHMD Link.exe" --fps`). pyopenvr
is bundled when it is installed (used to save SteamVR settings while it runs,
and by the keyboard/mouse tool to show SteamVR's poses).
"""

from __future__ import annotations

import argparse
import importlib.util
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CLIENTS = ROOT / "clients" / "python"
NAME = "VHMD Link"


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--onefile", action="store_true", help="a single exe instead of a folder")
    ap.add_argument("--out", type=Path, default=ROOT / "dist", help="output folder (default dist/)")
    args = ap.parse_args()
    if importlib.util.find_spec("PyInstaller") is None:
        raise SystemExit("PyInstaller is not installed: python -m pip install pyinstaller")

    sys.path.insert(0, str(CLIENTS))
    from vhmd_client.link import draw_icon

    work = ROOT / "build" / "pyinstaller"
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=work) as tmp:
        icon = draw_icon(Path(tmp))
        if icon is None:
            raise SystemExit("Pillow is needed to draw the icon: python -m pip install pillow")
        cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed",
               "--onefile" if args.onefile else "--onedir", "--name", NAME, "--icon", str(icon),
               "--add-data", f"{icon};.", "--add-data", f"{icon.with_suffix('.png')};.",
               "--paths", str(CLIENTS), "--distpath", str(args.out), "--workpath", str(work / "work"),
               "--specpath", str(work),
               # Not used by the app; keeps the bundle small.
               "--exclude-module", "numpy", "--exclude-module", "PIL", "--exclude-module", "pytest"]
        if importlib.util.find_spec("openvr") is not None:
            cmd += ["--collect-all", "openvr"]
        else:
            print("note: pyopenvr not installed; the exe works without it (settings are written to the file)")
        cmd.append(str(CLIENTS / "vhmd_link.pyw"))
        print("+", " ".join(cmd), flush=True)
        subprocess.run(cmd, check=True)
    exe = args.out / (f"{NAME}.exe" if args.onefile else f"{NAME}/{NAME}.exe")
    size = exe.stat().st_size if args.onefile else sum(p.stat().st_size for p in exe.parent.rglob("*") if p.is_file())
    print(f"built {exe} ({size / 1e6:.1f} MB{'' if args.onefile else ' folder'})")
    # PyInstaller leaves a copy of the spec; it is generated each time.
    shutil.rmtree(work / "work", ignore_errors=True)


if __name__ == "__main__":
    main()
