#!/usr/bin/env bash
# Detects the Linux development / test environment (read-only) and prints a report.

source "$(dirname "$0")/common.sh"
set +e

row() { printf '%-28s %s\n' "$1" "$2"; }

vr="$(steamvr_path)"
row steamvr_path "${vr:-NOT FOUND}"
if [ -n "$vr" ]; then
    row vrpathreg "$vr/bin/vrpathreg.sh"
    row steamvr_bin_version "$(head -1 "$vr/bin/version.txt" 2>/dev/null)"
    for acf in "$HOME/.steam/steam/steamapps/appmanifest_250820.acf" "$HOME/.local/share/Steam/steamapps/appmanifest_250820.acf"; do
        if [ -f "$acf" ]; then row steamvr_buildid "$(grep -oP '"buildid"\s+"\K[0-9]+' "$acf")"; break; fi
    done
fi
log="$(python3 -c 'import json,sys
print((json.load(open(sys.argv[1])).get("log") or [""])[0])' "$OPENVR_PATHS" 2>/dev/null)"
if [ -n "$log" ] && [ -f "${log%/}/vrserver.txt" ]; then
    row steamvr_version "$(grep -oP 'vrserver \K[0-9.]+(?= startup)' "${log%/}/vrserver.txt" | tail -1)"
fi
row openvrpaths "$OPENVR_PATHS"
echo "external_drivers:"
registered_drivers | sed 's/^/    /'
row steamvr_running "$(steamvr_running && echo yes || echo no)"
row distro "$(. /etc/os-release 2>/dev/null; echo "${PRETTY_NAME:-unknown}")"
row glibc "$(ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+$')"
# Prefer a real GPU over a VM's emulated display adapter.
gpus=$(lspci 2>/dev/null | grep -iE 'vga|3d|display')
row gpu "$( { echo "$gpus" | grep -iE 'nvidia|amd|ati |intel'; echo "$gpus"; } | head -1 | cut -d: -f3- | sed 's/^ //')"
row session_type "${XDG_SESSION_TYPE:-unknown}"
for t in g++ clang++ cmake ninja git python3; do
    row "$t" "$(command -v "$t" >/dev/null && "$t" --version 2>&1 | head -1 || echo MISSING)"
done
row pyopenvr "$(python3 -c 'import openvr;print(openvr.__version__)' 2>/dev/null || echo 'not installed (pip install openvr)')"
if [ -d "$REPO_ROOT/third_party/openvr/.git" ] || [ -f "$REPO_ROOT/third_party/openvr/.git" ]; then
    row openvr_sdk "$(git -C "$REPO_ROOT/third_party/openvr" describe --tags 2>/dev/null)"
fi
so="$(default_driver_path)/bin/linux64/driver_vhmd.so"
row driver_built "$([ -f "$so" ] && echo "yes ($so)" || echo no)"
if [ -f "$so" ]; then
    row driver_needs_glibc "$(objdump -T "$so" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)"
fi
row driver_registered "$(registered_drivers | grep -Fxq "$(default_driver_path)" && echo yes || echo no)"
