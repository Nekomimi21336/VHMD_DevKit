#!/usr/bin/env bash
# Unregisters the VirtualHMD driver from SteamVR (Linux).
#
# Usage: tools/uninstall_driver.sh [--clean-settings] [DRIVER_PATH]
#   DRIVER_PATH       folder to remove (default: every registered folder whose
#                     manifest names the driver "vhmd")
#   --clean-settings  also remove what the driver may have left in SteamVR's
#                     steamvr.vrsettings ("driver_vhmd", "vhmd_display" and
#                     /devices/vhmd/* tracker roles). A timestamped backup is
#                     written first. SteamVR must be closed.

source "$(dirname "$0")/common.sh"

CLEAN=0
TARGET=""
for a in "$@"; do
    case "$a" in
        --clean-settings) CLEAN=1 ;;
        *) TARGET="$(realpath -m "$a")" ;;
    esac
done

targets=()
if [ -n "$TARGET" ]; then
    targets=("$TARGET")
else
    while IFS= read -r d; do
        if is_vhmd_driver "$d" || { [ ! -e "$d" ] && [[ "$d" == */vhmd ]]; }; then targets+=("$d"); fi
    done < <(registered_drivers)
fi

if [ ${#targets[@]} -eq 0 ]; then
    echo "No registered vhmd driver folder found."
else
    steamvr_running && echo "warning: SteamVR is running; the change applies after it restarts." >&2
    for t in "${targets[@]}"; do
        vrpathreg removedriver "$t" && echo "Unregistered: $t"
    done
fi

if [ "$CLEAN" = 1 ]; then
    if steamvr_running; then
        echo "Close SteamVR before using --clean-settings (SteamVR rewrites the file on exit)." >&2
        exit 1
    fi
    # The config folder is listed in openvrpaths.vrpath ("config").
    cfg="$(python3 -c 'import json,sys
print((json.load(open(sys.argv[1])).get("config") or [""])[0])' "$OPENVR_PATHS" 2>/dev/null || true)"
    file="${cfg%/}/steamvr.vrsettings"
    if [ -z "$cfg" ] || [ ! -f "$file" ]; then
        echo "steamvr.vrsettings not found; nothing to clean."
    else
        python3 - "$file" <<'PY'
import json, shutil, sys, time
path = sys.argv[1]
with open(path, encoding="utf-8") as f:
    data = json.load(f)
changed = False
for section in ("driver_vhmd", "vhmd_display"):
    if section in data:
        del data[section]
        changed = True
trackers = data.get("trackers")
if isinstance(trackers, dict):
    for key in [k for k in trackers if k.startswith("/devices/vhmd/")]:
        del trackers[key]
        changed = True
if changed:
    backup = f"{path}.vhmd-backup-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copy2(path, backup)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=3, ensure_ascii=False)
    print(f"Cleaned vhmd entries from {path} (backup: {backup})")
else:
    print("No vhmd entries in steamvr.vrsettings.")
PY
    fi
fi

echo
vrpathreg show
