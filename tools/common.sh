# Shared helpers for the Linux tool scripts (source this file).
# Read-only discovery of Steam / SteamVR locations; nothing here modifies the system.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OPENVR_PATHS="${XDG_CONFIG_HOME:-$HOME/.config}/openvr/openvrpaths.vrpath"

# Prints the SteamVR runtime folder, or nothing.
steamvr_path() {
    # 1) the runtime SteamVR registered itself
    if [ -f "$OPENVR_PATHS" ] && command -v python3 >/dev/null; then
        local p
        p="$(python3 -c 'import json,sys,os
d=json.load(open(sys.argv[1]))
for r in d.get("runtime", []):
    if os.path.exists(os.path.join(r, "bin", "vrpathreg.sh")): print(r); break' "$OPENVR_PATHS" 2>/dev/null || true)"
        if [ -n "$p" ]; then echo "$p"; return; fi
    fi
    # 2) usual Steam library locations
    local lib
    for lib in "$HOME/.steam/steam" "$HOME/.local/share/Steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        if [ -x "$lib/steamapps/common/SteamVR/bin/vrpathreg.sh" ]; then
            echo "$lib/steamapps/common/SteamVR"; return
        fi
    done
}

vrpathreg() {
    local vr
    vr="$(steamvr_path)"
    if [ -z "$vr" ]; then echo "SteamVR installation not found" >&2; return 1; fi
    "$vr/bin/vrpathreg.sh" "$@"
}

# Prints registered external driver folders, one per line.
registered_drivers() {
    [ -f "$OPENVR_PATHS" ] || return 0
    python3 -c 'import json,sys
for d in json.load(open(sys.argv[1])).get("external_drivers") or []:
    print(d)' "$OPENVR_PATHS"
}

steamvr_running() {
    pgrep -x vrserver >/dev/null 2>&1 || pgrep -f "bin/linux64/vrserver" >/dev/null 2>&1
}

default_driver_path() {
    echo "$REPO_ROOT/build/driver/vhmd"
}

# True if the folder's manifest names the driver "vhmd".
is_vhmd_driver() {
    [ -f "$1/driver.vrdrivermanifest" ] && grep -Eq '"name"[[:space:]]*:[[:space:]]*"vhmd"' "$1/driver.vrdrivermanifest"
}
