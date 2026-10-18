#!/usr/bin/env bash
# Registers the VirtualHMD driver folder with SteamVR (Linux) via vrpathreg.sh.
# Nothing is copied into the SteamVR installation. Undo: tools/uninstall_driver.sh
#
# Usage: tools/install_driver.sh [DRIVER_PATH]    (default: build/driver/vhmd)

source "$(dirname "$0")/common.sh"

DRIVER_PATH="$(realpath -m "${1:-$(default_driver_path)}")"
if [ ! -f "$DRIVER_PATH/driver.vrdrivermanifest" ] || [ ! -f "$DRIVER_PATH/bin/linux64/driver_vhmd.so" ]; then
    echo "Driver not built at '$DRIVER_PATH'." >&2
    echo "Run: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build" >&2
    exit 1
fi

if steamvr_running; then
    echo "warning: SteamVR is running; the driver is loaded on the next SteamVR start." >&2
fi

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

echo
echo "Current SteamVR paths:"
vrpathreg show
