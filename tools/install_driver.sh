#!/usr/bin/env bash
# Registers the VirtualHMD driver folder with SteamVR (Linux) via vrpathreg.sh.
#
# Before changing anything, copies the SteamVR files the driver can affect
# (openvrpaths.vrpath, steamvr.vrsettings, chaperone_info.vrchap) and the list
# of registered drivers into a record under
# ${XDG_STATE_HOME:-~/.local/state}/vhmd_devkit/history/<time>-install.
# Nothing is copied into the SteamVR installation.
# Undo: tools/uninstall_driver.sh (--restore puts the recorded files back)
#
# Usage: tools/install_driver.sh [DRIVER_PATH]
#   default: the bundled vhmd folder of a release archive, else build/driver/vhmd

source "$(dirname "$0")/common.sh"

DRIVER_PATH="$(realpath -m "${1:-$(default_driver_path)}")"
if [ ! -f "$DRIVER_PATH/driver.vrdrivermanifest" ] || [ ! -f "$DRIVER_PATH/bin/linux64/driver_vhmd.so" ]; then
    echo "Driver not built at '$DRIVER_PATH'." >&2
    echo "Run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    exit 1
fi

running=()
if steamvr_running; then
    echo "warning: SteamVR is running; the driver is loaded on the next SteamVR start." >&2
    running=(--running)
fi

vr="$(steamvr_path)"
if [ -z "$vr" ]; then echo "SteamVR installation not found" >&2; exit 1; fi
record="$(vhmd_records save install --driver "$DRIVER_PATH" --steamvr "$vr" "${running[@]}")"
echo "Recorded the current SteamVR setup: $record"

if registered_drivers | grep -Fxq "$DRIVER_PATH"; then
    echo "Already registered: $DRIVER_PATH"
else
    while IFS= read -r d; do
        if [ "$d" != "$DRIVER_PATH" ] && is_vhmd_driver "$d"; then
            echo "warning: another vhmd driver folder is registered: $d (remove it with uninstall_driver.sh '$d')" >&2
        fi
    done < <(registered_drivers)
    vrpathreg adddriver "$DRIVER_PATH"
    echo "Registered: $DRIVER_PATH"
fi
vhmd_records after "$record"

echo
echo "Current SteamVR paths:"
vrpathreg show
