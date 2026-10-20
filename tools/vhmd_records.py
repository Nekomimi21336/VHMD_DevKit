#!/usr/bin/env python3
"""Install records for the Linux tool scripts (install_driver.sh / uninstall_driver.sh).

Before the scripts change anything they copy the SteamVR files the driver can
affect (openvrpaths.vrpath, steamvr.vrsettings, chaperone_info.vrchap) and the
list of registered drivers into

    ${XDG_STATE_HOME:-~/.local/state}/vhmd_devkit/history/<time>-<kind>/

uninstall_driver.sh uses the oldest open install record to report what changed
since, or (--restore) to put the files back. Records are never deleted.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
import re
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOME = Path.home()
OPENVR_PATHS = Path(os.environ.get("XDG_CONFIG_HOME") or HOME / ".config") / "openvr" / "openvrpaths.vrpath"
HISTORY = Path(os.environ.get("XDG_STATE_HOME") or HOME / ".local" / "state") / "vhmd_devkit" / "history"
STEAM_ROOTS = [HOME / ".steam/steam", HOME / ".local/share/Steam", HOME / ".var/app/com.valvesoftware.Steam/.local/share/Steam"]
VHMD_SECTIONS = ("driver_vhmd", "vhmd_display")


def read_json(path: Path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_json(path: Path, data, indent=2) -> None:
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=indent, ensure_ascii=False)
        f.write("\n")


def openvr_paths() -> dict:
    try:
        return read_json(OPENVR_PATHS)
    except (OSError, ValueError):
        return {}


def external_drivers() -> list[str]:
    return [d for d in openvr_paths().get("external_drivers") or [] if d]


def config_dir() -> Path | None:
    cfg = (openvr_paths().get("config") or [None])[0]
    if cfg:
        return Path(cfg)
    for root in STEAM_ROOTS:
        if (root / "config").is_dir():
            return root / "config"
    return None


def recorded_files() -> dict[str, Path]:
    """Name inside a record -> current location."""
    files = {"openvrpaths.vrpath": OPENVR_PATHS}
    cfg = config_dir()
    if cfg:
        files["steamvr.vrsettings"] = cfg / "steamvr.vrsettings"
        files["chaperone_info.vrchap"] = cfg / "chaperone_info.vrchap"
    return files


def steamvr_buildid() -> str | None:
    for root in STEAM_ROOTS:
        acf = root / "steamapps" / "appmanifest_250820.acf"
        if acf.is_file():
            m = re.search(r'"buildid"\s+"([^"]+)"', acf.read_text(errors="replace"))
            return m.group(1) if m else None
    return None


def vhmd_version() -> str:
    try:
        m = re.search(r"^## v([0-9][0-9.]*)", (ROOT / "CHANGELOG.md").read_text(encoding="utf-8"), re.M)
        return m.group(1) if m else "unknown"
    except OSError:
        return "unknown"


def is_vhmd_key(section: str, key: str | None) -> bool:
    return section in VHMD_SECTIONS or (section == "trackers" and key is not None and key.startswith("/devices/vhmd/"))


def load_record(path: str | Path) -> dict:
    d = Path(path).resolve()
    rec = read_json(d / "record.json")
    rec["dir"] = str(d)
    return rec


def save_field(rec: dict, name: str, value) -> None:
    rec[name] = value
    write_json(Path(rec["dir"]) / "record.json", {k: v for k, v in rec.items() if k != "dir"})


def records() -> list[dict]:
    if not HISTORY.is_dir():
        return []
    return [load_record(d) for d in sorted(HISTORY.iterdir()) if (d / "record.json").is_file()]


def open_installs() -> list[dict]:
    return [r for r in records() if r.get("kind") == "install" and not r.get("closed")]


# ---- commands ----------------------------------------------------------------

def cmd_save(a) -> None:
    stamp = dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    d = HISTORY / f"{stamp}-{a.kind}"
    n = 2
    while d.exists():
        d = HISTORY / f"{stamp}-{a.kind}-{n}"
        n += 1
    d.mkdir(parents=True)
    files = {}
    for name, src in recorded_files().items():
        info = {"path": str(src), "existed": src.is_file()}
        if info["existed"]:
            shutil.copy2(src, d / name)
            info["sha256"] = hashlib.sha256(src.read_bytes()).hexdigest().upper()
        files[name] = info
    rec = {
        "kind": a.kind,
        "created": dt.datetime.now().astimezone().isoformat(),
        "vhmd_version": vhmd_version(),
        "driver_path": a.driver or None,
        "steamvr_path": a.steamvr or None,
        "steamvr_buildid": steamvr_buildid(),
        "steamvr_running": a.running,
        "external_drivers": external_drivers(),
        "files": files,
    }
    if a.driver and Path(a.driver).is_dir():
        root = Path(a.driver)
        listing = sorted(p.relative_to(root).as_posix() for p in root.rglob("*") if p.is_file())
        (d / "driver_files.txt").write_text("".join(f"{p}\n" for p in listing), encoding="utf-8")
    write_json(d / "record.json", rec)
    print(d)


def cmd_after(a) -> None:
    save_field(load_record(a.record), "external_drivers_after", external_drivers())


def cmd_reference(a) -> None:
    if a.record:
        print(load_record(a.record)["dir"])
    else:
        opened = open_installs()
        if opened:
            print(opened[0]["dir"])


def cmd_field(a) -> None:
    value = load_record(a.record).get(a.name)
    for v in value if isinstance(value, list) else [value]:
        if v not in (None, ""):
            print(str(v).lower() if isinstance(v, bool) else v)


def cmd_clean(a) -> None:
    path = recorded_files().get("steamvr.vrsettings")
    if not path or not path.is_file():
        print("steamvr.vrsettings not found; nothing to clean.")
        return
    data = read_json(path)
    changed = False
    for section in VHMD_SECTIONS:
        if section in data:
            del data[section]
            changed = True
    trackers = data.get("trackers")
    if isinstance(trackers, dict):
        for key in [k for k in trackers if is_vhmd_key("trackers", k)]:
            del trackers[key]
            changed = True
    if changed:
        write_json(path, data, indent=3)
        print(f"Removed the vhmd entries from {path}")
    else:
        print("No vhmd entries in steamvr.vrsettings.")


def cmd_restore_files(a) -> None:
    rec = load_record(a.record)
    current = recorded_files()
    for name in ("steamvr.vrsettings", "chaperone_info.vrchap"):
        info = rec["files"].get(name)
        dest = current.get(name)
        if not info or not dest:
            continue
        if info["existed"]:
            shutil.copy2(Path(rec["dir"]) / name, dest)
            print(f"Restored: {dest}")
        elif dest.exists():
            dest.unlink()
            print(f"Removed (did not exist when recorded): {dest}")
    if rec.get("steamvr_running"):
        print("warning: SteamVR was running when that record was made; its settings file may not have been final.", file=sys.stderr)


def cmd_diff(a) -> None:
    rec = load_record(a.record)
    before_path = Path(rec["dir"]) / "steamvr.vrsettings"
    after_path = recorded_files().get("steamvr.vrsettings")
    before = read_json(before_path) if before_path.is_file() else {}
    after = read_json(after_path) if after_path and after_path.is_file() else {}
    out = []
    for s in dict.fromkeys([*before, *after]):
        if is_vhmd_key(s, None):
            continue
        if s not in before:
            out.append(f"+ {s}")
        elif s not in after:
            out.append(f"- {s}")
        elif isinstance(before[s], dict) and isinstance(after[s], dict):
            for k in dict.fromkeys([*before[s], *after[s]]):
                if is_vhmd_key(s, k):
                    continue
                if k not in before[s]:
                    out.append(f"+ {s}.{k}")
                elif k not in after[s]:
                    out.append(f"- {s}.{k}")
                elif before[s][k] != after[s][k]:
                    out.append(f"~ {s}.{k}")
        elif before[s] != after[s]:
            out.append(f"~ {s}")
    was, now, removed = rec.get("external_drivers") or [], external_drivers(), set(a.removed)
    out += [f"+ driver {d}" for d in now if d not in was]
    out += [f"- driver {d}" for d in was if d not in now and d not in removed]
    print()
    if out:
        print("Differences from the record (left as they are; --restore puts the recorded state back):")
        for line in out:
            print(f"  {line}")
    else:
        print("Everything else matches the record.")


def cmd_close(a) -> None:
    stamp = dt.datetime.now().astimezone().isoformat()
    removed = set(a.removed)
    for rec in open_installs():
        if a.all or rec.get("driver_path") in removed:
            save_field(rec, "closed", stamp)


def cmd_remove_generated(a) -> None:
    res = Path(a.driver) / "resources"
    items = []
    if (res / "icons" / "labels").is_dir():
        items.append(res / "icons" / "labels")
    if (res / "rendermodels").is_dir():
        items += sorted(res.glob("rendermodels/vhmd_vhmd-*"))
    if (res / "icons").is_dir():
        items += [p for p in (res / "icons").iterdir() if p.is_file() and re.search(r"\.[0-9a-f]{8}\.(png|gif)$", p.name)]
    for p in items:
        shutil.rmtree(p) if p.is_dir() else p.unlink()
    if items:
        print(f"Deleted {len(items)} generated item(s) in {res}")


def cmd_list(a) -> None:
    recs = records()
    if not recs:
        print(f"No records in {HISTORY}")
        return
    for r in recs:
        closed = f"  closed {r['closed']}" if r.get("closed") else ""
        print(f"{Path(r['dir']).name}  v{r.get('vhmd_version')}  {r.get('driver_path') or '-'}{closed}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("save", help="record the current SteamVR files; prints the record folder")
    p.add_argument("kind")
    p.add_argument("--driver", default="")
    p.add_argument("--steamvr", default="")
    p.add_argument("--running", action="store_true")
    p.set_defaults(fn=cmd_save)
    p = sub.add_parser("after", help="store the driver list after installing")
    p.add_argument("record")
    p.set_defaults(fn=cmd_after)
    p = sub.add_parser("reference", help="print the record to compare with")
    p.add_argument("--record")
    p.set_defaults(fn=cmd_reference)
    p = sub.add_parser("field", help="print a record field (lists one item per line)")
    p.add_argument("record")
    p.add_argument("name")
    p.set_defaults(fn=cmd_field)
    sub.add_parser("clean", help="remove the vhmd entries from steamvr.vrsettings").set_defaults(fn=cmd_clean)
    p = sub.add_parser("restore-files", help="put steamvr.vrsettings and chaperone_info.vrchap back")
    p.add_argument("record")
    p.set_defaults(fn=cmd_restore_files)
    p = sub.add_parser("diff", help="report what differs from the record")
    p.add_argument("record")
    p.add_argument("removed", nargs="*")
    p.set_defaults(fn=cmd_diff)
    p = sub.add_parser("close", help="mark install records as answered by an uninstall")
    p.add_argument("--all", action="store_true")
    p.add_argument("removed", nargs="*")
    p.set_defaults(fn=cmd_close)
    p = sub.add_parser("remove-generated", help="delete files generated inside a driver folder")
    p.add_argument("driver")
    p.set_defaults(fn=cmd_remove_generated)
    sub.add_parser("list", help="show the records").set_defaults(fn=cmd_list)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
