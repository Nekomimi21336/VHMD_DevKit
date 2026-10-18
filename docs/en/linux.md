[日本語](../linux.md) | **English** | [简体中文](../zh-CN/linux.md) | [한국어](../ko/linux.md) — [Back to README](../../README.en.md)

# Using with SteamVR for Linux

These are the steps to load `driver_vhmd.so` into native Linux SteamVR. Windows games running under Proton (VRChat, etc.) also connect to SteamVR for Linux, so they need this driver too. The sender-side protocol and the Python client are the same as on Windows.

## Status (as of 2026-10-18)

| Item | Status |
|---|---|
| Linux build (gcc 13.3, Ubuntu 24.04 / WSL2) | ✅ 0 warnings (`-Wall -Wextra -Wpedantic`) |
| 36 C++ unit tests | ✅ All pass (including UDP loopback) |
| Exported symbols | ✅ `HmdDriverFactory` only (linker version script) |
| Library dependencies | ✅ `libc`, `libm`, `ld-linux` only (libstdc++ / libgcc statically linked) |
| `tools/*.sh` | ✅ install / uninstall / `--clean-settings` checked against a fake SteamVR environment |
| Linux support in integration tests (`tests/integration/steamvr_util.py`) | ⚠ Only loading and process detection checked on WSL |
| **Operation on real SteamVR for Linux** | ❌ **Unverified** (this development PC does not have SteamVR for Linux) |

## Prerequisites

- x86_64 Linux, Steam + SteamVR (native Linux version)
- g++ 9 or later (or clang), CMake 3.20 or later, Git, Python 3.10 or later
- Ubuntu / Debian example: `sudo apt install build-essential cmake ninja-build git python3`

## Build

```bash
git clone --recurse-submodules <this repository>
cd VirtualHMD
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Output: `build/driver/vhmd/bin/linux64/driver_vhmd.so` (+ `driver.vrdrivermanifest`, `resources/`).

### Note on glibc

The resulting `.so` requires **a glibc version at least as new as the build environment's** (built on Ubuntu 24.04, it needs glibc 2.38 or later). You can check this with `driver_needs_glibc` in `tools/verify_env.sh`. **Building on the PC that runs SteamVR itself** is the most reliable approach. If you also distribute to older distributions, build in an older environment (or a Steam Runtime SDK container).

## Register / unregister

```bash
tools/verify_env.sh            # check the environment (read-only)
tools/install_driver.sh        # vrpathreg.sh adddriver build/driver/vhmd
tools/uninstall_driver.sh --clean-settings   # unregister + remove vhmd entries from SteamVR settings (with backup)
```

- `vrpathreg.sh` is `<SteamVR>/bin/vrpathreg.sh`. The SteamVR location is taken from `runtime` in `~/.config/openvr/openvrpaths.vrpath`; if absent, it searches `~/.steam/steam`, `~/.local/share/Steam`, and Flatpak, in that order.
- Nothing is copied into the SteamVR installation folder.
- **Quit SteamVR before rebuilding.**

## Usage

```bash
python3 clients/python/examples/fullbody_sway.py
```

Log: `~/.steam/steam/logs/vrserver.txt` (location given by `log` in `openvrpaths.vrpath`). Lines starting with `vhmd:` come from this driver.

## Biggest risk: the virtual display

The HMD is displayed as "a window on the desktop" via `IVRDisplayComponent` (confirmed working on Windows; same approach as the OpenVR sample `simplehmd`). **Whether the SteamVR for Linux compositor starts with this approach is unknown.** Symptoms and remedies if it does not start:

| Symptom | Remedy |
|---|---|
| SteamVR reports "headset not found / compositor does not start" | First use only the other features with `hmd.enabled: false` (tracker/controller-only mode used alongside a real HMD) |
| You want the HMD to be virtual too on a PC without a real HMD | Implement `IVRVirtualDisplay` (replace the component returned by `HmdDevice::GetComponent`; see the extension points in `docs/architecture.md`) |
| Vulkan / display errors in the `vrcompositor` log | Make `windowWidth/Height` in `vhmd_display` fit within the screen; on Wayland, also try an X11 session |

## On-hardware verification checklist

Fill in the results and reflect them in `docs/verification.md` (Japanese).

| # | Step | Expected result | Result |
|---|---|---|---|
| L1 | `tools/verify_env.sh` | SteamVR path, glibc ≥ `driver_needs_glibc` | Unverified |
| L2 | `tools/install_driver.sh` → start SteamVR | `Loaded server driver vhmd` and `vhmd: listening on udp://127.0.0.1:39570` in `vrserver.txt` | Unverified |
| L3 | Same log | `Active HMD set to vhmd.VHMD-HMD-0` (when no real HMD is connected). Whether the compositor starts (risk above) | Unverified |
| L4 | `pip install openvr` → `python3 tests/integration/run_integration.py` | The same 29 items as on Windows. `--stop-relaunchers` terminates auto-launching overlays, so use it only when needed | Unverified |
| L5 | `python3 clients/python/examples/fullbody_sway.py` + SteamVR's "Display VR View" | The viewpoint and devices move | Unverified |
| L6 | Launch VRChat in VR mode via Proton | HMD, hands, and trackers are reflected | Unverified |
| L7 | `hmd.enabled: false` + real HMD | Controllers/trackers are added alongside the real HMD | Unverified |
| L8 | Quit SteamVR | `vhmd: shutting down` appears and vrserver exits (no hang) | Unverified |

## Changes made for Linux support

- `driver/driver_exports.map` + `-Wl,--version-script`: hides everything except `HmdDriverFactory` (vrserver loads multiple drivers into the same process, so this prevents symbol collisions).
- `-static-libstdc++ -static-libgcc`: so the driver loads even if the SteamVR runtime's libstdc++ is old. `-Wl,--no-undefined` detects unresolved symbols at build time.
- Fixed 4 gcc-specific warnings (initialization, checking the return value when reading OSC bundle lengths, reference to a temporary object).
- `.gitattributes`: forces LF for `*.sh` (so Windows `core.autocrlf` does not break them).
- `tests/integration/steamvr_util.py`: process detection via `/proc`, stopping via signals, launching with `vrstartup.sh`, `~/.steam` / XDG paths. OpenVR clients are detected as processes that have loaded `vrclient.so`; Steam itself and anything under `steamapps` (including Proton games) are excluded.
