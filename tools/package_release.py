"""Builds the release archives from already-built driver folders.

    python tools/package_release.py --version 0.2.0 \
        --win64 build/driver/vhmd --linux64 build-linux/driver/vhmd

Each archive contains a ready-to-register `vhmd/` driver folder (only the
platform's binary), the matching install/uninstall tools, the Python client
and examples, docs and licenses:

    VHMD_DevKit-<version>-win64.zip
    VHMD_DevKit-<version>-linux64.tar.gz

Run tools/install_driver.(ps1|sh) inside the extracted folder; it picks up the
bundled vhmd/ folder automatically. Output goes to dist/ with SHA256SUMS.txt.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

COMMON = [
    "README.md", "README.en.md", "README.zh-CN.md", "README.ko.md",
    "LICENSE", "THIRD_PARTY_NOTICES.md",
    "docs", "clients/python",
]
TOOLS = {
    "win64": ["tools/common.ps1", "tools/install_driver.ps1", "tools/uninstall_driver.ps1", "tools/verify_env.ps1"],
    "linux64": ["tools/common.sh", "tools/install_driver.sh", "tools/uninstall_driver.sh", "tools/verify_env.sh"],
}
BINARY = {"win64": "driver_vhmd.dll", "linux64": "driver_vhmd.so"}
SKIP_PARTS = {"__pycache__", ".pytest_cache"}


def collect(platform: str, driver_dir: Path) -> list[tuple[str, Path]]:
    """[(archive path, source file)]"""
    driver_dir = driver_dir.resolve()
    binary = driver_dir / "bin" / platform / BINARY[platform]
    if not binary.exists():
        raise SystemExit(f"missing {binary}")
    # Only the binary comes from the build output. Manifest and resources come
    # from the source tree: SteamVR writes hashed icon copies into a driver
    # folder it has loaded, and a staged config may have local edits.
    files: list[tuple[str, Path]] = [(f"vhmd/bin/{platform}/{BINARY[platform]}", binary)]
    src_driver = ROOT / "driver"
    files.append(("vhmd/driver.vrdrivermanifest", src_driver / "driver.vrdrivermanifest"))
    for src in sorted((src_driver / "resources").rglob("*")):
        if src.is_file():
            files.append((f"vhmd/{src.relative_to(src_driver).as_posix()}", src))
    for item in COMMON + TOOLS[platform]:
        p = ROOT / item
        srcs = [p] if p.is_file() else sorted(x for x in p.rglob("*") if x.is_file())
        for s in srcs:
            if SKIP_PARTS & set(s.parts) or s.suffix == ".pyc":
                continue
            files.append((s.relative_to(ROOT).as_posix(), s))
    return files


def is_executable(arcname: str) -> bool:
    return arcname.endswith((".sh", ".so")) or arcname.startswith("clients/python/examples/")


def write_zip(out: Path, prefix: str, files) -> None:
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for arc, src in files:
            z.write(src, f"{prefix}/{arc}")


def write_tar(out: Path, prefix: str, files) -> None:
    with tarfile.open(out, "w:gz", format=tarfile.PAX_FORMAT) as t:
        for arc, src in files:
            data = src.read_bytes()
            if arc.endswith(".sh"):
                data = data.replace(b"\r\n", b"\n")  # never ship CRLF shell scripts
            info = tarfile.TarInfo(f"{prefix}/{arc}")
            info.size = len(data)
            info.mode = 0o755 if is_executable(arc) else 0o644
            info.mtime = int(src.stat().st_mtime)
            t.addfile(info, io.BytesIO(data))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True)
    ap.add_argument("--win64", type=Path, help="built Windows driver folder (contains bin/win64/driver_vhmd.dll)")
    ap.add_argument("--linux64", type=Path, help="built Linux driver folder (contains bin/linux64/driver_vhmd.so)")
    ap.add_argument("--out", type=Path, default=ROOT / "dist")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    made = []
    for platform, folder in (("win64", args.win64), ("linux64", args.linux64)):
        if folder is None:
            continue
        prefix = f"VHMD_DevKit-{args.version}-{platform}"
        files = collect(platform, folder)
        out = args.out / (prefix + (".zip" if platform == "win64" else ".tar.gz"))
        (write_zip if platform == "win64" else write_tar)(out, prefix, files)
        made.append(out)
        print(f"{out.name}: {len(files)} files, {out.stat().st_size / 1024:.0f} KiB")
    sums = "".join(f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}\n" for p in made)
    # LF endings so `sha256sum -c` works on Linux too
    (args.out / "SHA256SUMS.txt").write_bytes(sums.encode("utf-8"))
    print(sums, end="")


if __name__ == "__main__":
    main()
