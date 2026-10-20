#!/usr/bin/env bash
# Unregisters the VirtualHMD driver from SteamVR (Linux) and cleans up after it.
#
# First records the current SteamVR files (like install_driver.sh, as
# .../vhmd_devkit/history/<time>-uninstall), so this step can be undone too.
# Then, by default:
#   - unregisters every registered folder whose manifest names the driver "vhmd",
#   - deletes the files the driver and SteamVR generated inside those folders
#     (labelled render models, label icons, SteamVR's hashed icon copies),
#   - removes the driver's entries from steamvr.vrsettings ("driver_vhmd",
#     "vhmd_display", tracker roles under /devices/vhmd/),
#   - lists what else changed since the install record, without touching it.
#
# Usage: tools/uninstall_driver.sh [options] [DRIVER_PATH]
#   DRIVER_PATH            only unregister this folder
#   --restore              put registrations, steamvr.vrsettings and
#                          chaperone_info.vrchap back exactly as recorded at
#                          install time (SteamVR must be closed)
#   --record DIR           record to compare with / restore from (default: the
#                          oldest install record not yet followed by an
#                          uninstall; an uninstall record undoes an uninstall)
#   --keep-settings        leave steamvr.vrsettings as it is
#   --keep-generated-files leave the generated files in the driver folder
#   --list                 show the records and exit
#   --clean-settings       accepted for compatibility (now the default)

source "$(dirname "$0")/common.sh"

RESTORE=0 KEEP_SETTINGS=0 KEEP_GENERATED=0 RECORD="" TARGET=""
while [ $# -gt 0 ]; do
    case "$1" in
        --restore) RESTORE=1 ;;
        --record) RECORD="$(realpath -m "$2")"; shift ;;
        --keep-settings) KEEP_SETTINGS=1 ;;
        --keep-generated-files) KEEP_GENERATED=1 ;;
        --clean-settings) ;;
        --list) vhmd_records list; exit 0 ;;
        -*) echo "unknown option: $1" >&2; exit 2 ;;
        *) TARGET="$(realpath -m "$1")" ;;
    esac
    shift
done

if [ -n "$RECORD" ]; then ref="$(vhmd_records reference --record "$RECORD")"; else ref="$(vhmd_records reference)"; fi
running=0
steamvr_running && running=1
if [ "$RESTORE" = 1 ]; then
    [ -n "$ref" ] || { echo "No install record found; nothing to restore." >&2; exit 1; }
    [ "$running" = 0 ] || { echo "Close SteamVR before using --restore (SteamVR rewrites its settings on exit)." >&2; exit 1; }
fi
if [ -n "$ref" ]; then echo "Reference record: $ref"; else echo "No install record found; cleaning up without one."; fi

undo_flags=()
[ "$running" = 1 ] && undo_flags=(--running)
undo="$(vhmd_records save uninstall --steamvr "$(steamvr_path)" "${undo_flags[@]}")"
echo "Recorded the current SteamVR setup: $undo"
[ "$running" = 1 ] && echo "warning: SteamVR is running; registration changes apply after it restarts." >&2

# Driver registrations.
removed=()
if [ "$RESTORE" = 1 ]; then
    mapfile -t want < <(vhmd_records field "$ref" external_drivers)
    while IFS= read -r d; do
        if ! printf '%s\n' "${want[@]}" | grep -Fxq "$d"; then
            vrpathreg removedriver "$d" && { echo "Unregistered: $d"; removed+=("$d"); }
        fi
    done < <(registered_drivers)
    for d in "${want[@]}"; do
        registered_drivers | grep -Fxq "$d" && continue
        if [ ! -e "$d" ]; then echo "warning: recorded driver folder no longer exists, not registered again: $d" >&2; continue; fi
        vrpathreg adddriver "$d" && echo "Registered again: $d"
    done
else
    targets=()
    if [ -n "$TARGET" ]; then
        targets=("$TARGET")
    else
        while IFS= read -r d; do
            if is_vhmd_driver "$d" || { [ ! -e "$d" ] && [[ "$d" == */vhmd ]]; }; then targets+=("$d"); fi
        done < <(registered_drivers)
    fi
    [ ${#targets[@]} -eq 0 ] && echo "No registered vhmd driver folder found."
    for t in "${targets[@]}"; do
        vrpathreg removedriver "$t" && { echo "Unregistered: $t"; removed+=("$t"); }
    done
fi

# Files generated inside the unregistered vhmd folders.
if [ "$KEEP_GENERATED" = 0 ]; then
    for t in "${removed[@]}"; do
        is_vhmd_driver "$t" && vhmd_records remove-generated "$t"
    done
fi

# SteamVR settings, then what else differs from the record (reported only).
if [ "$RESTORE" = 1 ]; then
    vhmd_records restore-files "$ref"
else
    if [ "$KEEP_SETTINGS" = 0 ]; then
        if [ "$running" = 1 ]; then
            echo "warning: SteamVR is running; the vhmd entries in steamvr.vrsettings were left. Close SteamVR and run this again." >&2
        else
            vhmd_records clean
        fi
    fi
    [ -n "$ref" ] && vhmd_records diff "$ref" "${removed[@]}"
fi

# Close the install records this uninstall answers.
if [ "$RESTORE" = 1 ]; then vhmd_records close --all; else vhmd_records close "${removed[@]}"; fi

echo
vrpathreg show
