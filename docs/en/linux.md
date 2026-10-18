[日本語](../linux.md) | **English** | [简体中文](../zh-CN/linux.md) | [한국어](../ko/linux.md) — [Back to README](../../README.en.md)

# Using with SteamVR for Linux

These are the steps to load `driver_vhmd.so` into native Linux SteamVR. Windows games running under Proton (VRChat and others) also connect to SteamVR for Linux, so they need this driver too. The sender-side protocol and the Python client are the same as on Windows.

## How it works: on Linux the driver runs in "direct mode"

Unlike on Windows, the SteamVR for Linux compositor cannot draw into "a window on the desktop". If there is no display it can acquire in direct mode (DRM lease), it aborts startup with `VRInitError_Compositor_CannotDRMLeaseDisplay`. The same happens with the null driver bundled with SteamVR, and Valve treats it as a known issue ([SteamVR-for-Linux#889](https://github.com/ValveSoftware/SteamVR-for-Linux/issues/889)).

Therefore, starting with v0.3.0, the virtual HMD on Linux implements **`IVRDriverDirectModeComponent`**.

```
app ──▶ vrcompositor ──(shared Vulkan image / dma-buf)──▶ driver_vhmd.so
                                                         ├─ signals vsync itself (default 90 Hz)
                                                         └─ writes the latest frame to /dev/shm/vhmd_preview (optional)
```

- Neither a real display nor a DRM lease is required. The left and right eye images are rendered into shared Vulkan images allocated by the driver and handed over in `Present` (a Linux-only API of `IVRIPCResourceManagerClient`).
- This is the same approach as ALVR's Linux driver (master as of 2026-10). No code was copied; it was implemented following the OpenVR headers.
- Windows keeps using the desktop window approach as before (`display.mode`: `auto` / `window` / `direct`).

## Status (as of 2026-10-18)

| Item | Status |
|---|---|
| Linux build (gcc 13.3, Ubuntu 24.04 / WSL2) | ✅ 0 warnings (`-Wall -Wextra -Wpedantic`) |
| 37 C++ unit tests | ✅ All pass (no issues under ASan / TSan either) |
| Frame capture self-test (`tests/linux/capture_selftest.cpp`) | ✅ Verified with real Vulkan (Mesa llvmpipe), from fd import → downscale → shared memory → pixel values |
| Exported symbols / library dependencies | ✅ `HmdDriverFactory` only / `libc`, `libm`, `ld-linux` only (libvulkan is loaded at runtime with `dlopen`) |
| v0.2.0 run on SteamVR for Linux 2.17.10 (Arch, KDE Wayland) | ✅ Driver loads, HMD recognized, controller remapping / ❌ Compositor fails to start with `CannotDRMLeaseDisplay` (same with the null driver) → addressed by direct mode in v0.3.0 |
| **v0.3.0 (direct mode) run on SteamVR for Linux** | ❌ **Unverified**. Please check with `linux_check.py` below |

## Prerequisites

- x86_64 Linux, Steam + SteamVR (native Linux version)
- g++ 9 or later (or clang), CMake 3.20 or later, Git, Python 3.10 or later
- A Vulkan driver (already installed if SteamVR runs). The Vulkan headers (`vulkan-headers` / `libvulkan-dev`) are fetched automatically at configure time if missing
- Ubuntu / Debian example: `sudo apt install build-essential cmake ninja-build git python3`
- Arch example: `sudo pacman -S base-devel cmake ninja git python vulkan-headers`

## Build

```bash
git clone --recurse-submodules https://github.com/Nekomimi21336/VHMD_DevKit.git
cd VHMD_DevKit
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Output: `build/driver/vhmd/bin/linux64/driver_vhmd.so` (+ `driver.vrdrivermanifest`, `resources/`).

### Note on glibc

The resulting `.so` requires **a glibc version at least as new as the build environment's** (built on Ubuntu 24.04, it needs glibc 2.38 or later). You can check this with `driver_needs_glibc` in `tools/verify_env.sh`. **Building on the PC that runs SteamVR itself** is the most reliable approach.

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
python3 clients/python/examples/fullbody_sway.py --points 6
```

To see what SteamVR is rendering to the virtual HMD:

```bash
python3 clients/python/examples/preview_viewer.py                 # show in a window (standard library only)
python3 clients/python/examples/preview_viewer.py --snapshot eyes.png
```

To read it from your own tools, use `vhmd_client.preview.PreviewReader` (format: `driver/src/util/preview_shm.h`; RGBA8 with the left and right eyes side by side, sRGB).

| Setting (`preview` in `vhmd_config.json`) | Default | Description |
|---|---|---|
| `enabled` | `true` | Write to shared memory |
| `path` | `/dev/shm/vhmd_preview` | Output location |
| `fps` | `15` | Maximum write rate (independent of the compositor's rendering rate) |
| `maxEyeWidth` | `960` | Maximum width per eye (frames are downscaled before writing) |
| `vulkanDevice` | `-1` | Index of the GPU to use. `-1` means the first GPU. **It must be the same GPU SteamVR uses** (change it if no frames arrive on a multi-GPU PC) |

If preview processing fails, the preview is simply disabled; rendering to SteamVR is not affected.

Log: `~/.steam/steam/logs/vrserver.txt` (location given by `log` in `openvrpaths.vrpath`). Lines starting with `vhmd:` come from this driver. The `display` field of the `config list` response contains the mode, present count, vsync count, and preview state.

## On-hardware verification (automated)

```bash
pip install openvr
python3 tests/integration/linux_check.py
```

This automates everything from starting SteamVR to shutting it down, and writes a report (`report.md`, `report.json`), a preview snapshot (`eyes.png`), and log excerpts to `tests/integration/results/linux_check_<datetime>/`. **If you send us this folder, we can tell what is going on.** What it checks:

| # | Check | Expected |
|---|---|---|
| 1 | Environment (distro, session type, GPU, Vulkan, SteamVR build and branch) | Info |
| 2 | Building and registering the driver | ✅ |
| 3 | SteamVR loads the driver and it responds over UDP | ✅ |
| 4 | The HMD is in direct mode and vsync is running | ✅ |
| 5 | The compositor creates swap textures and presents at least 30 times per second | ✅ This is the key check |
| 6 | No `CannotDRMLeaseDisplay` or similar in `vrcompositor.txt` | ✅ |
| 7 | The preview has advancing frames, and `eyes.png` is saved | ✅ |
| 8 | The regular integration tests (`run_integration.py`: enumeration, poses, input, skeleton, load) | ✅ |
| 9 | The driver's shutdown runs when SteamVR quits | ✅ |

`--skip-integration` skips 8 (when you only want to check the display path).

## Troubleshooting

| Symptom | What to check |
|---|---|
| 5 is 0 (no presents arrive) | The end of `vrcompositor.txt`. The SteamVR version (ALVR may have been tested on the beta, and whether it works the same way on stable 2.17.10 is unconfirmed). Whether `steamvr.vrsettings` still contains launch options or `forcedDriver` left over from ALVR or similar |
| Presents arrive but the preview is black / missing | `display.preview.state` in `report.json`. On multi-GPU systems, `preview.vulkanDevice` |
| VRChat does not start | Try it only after 1–7 are all ✅. Proton's log (`PROTON_LOG=1`) |
| You want to use it alongside a real HMD | `"hmd": {"enabled": false}` (adds only controllers / trackers) |

## Changes made for Linux support

- **v0.3.0**: `driver/src/linux/` (`direct_mode.*`: the direct mode component and vsync clock, `frame_capture.*`: importing the shared images and downscaled copying, `vk_loader.*`: dynamic loading of libvulkan), `util/preview_shm.h`, Python `preview.py` / `preview_viewer.py`, `tests/linux/capture_selftest.cpp`, `tests/integration/linux_check.py`.
- `driver/driver_exports.map` + `-Wl,--version-script`: hides everything except `HmdDriverFactory` (because vrserver loads multiple drivers into the same process).
- `-static-libstdc++ -static-libgcc`: so the driver loads even if the SteamVR runtime's libstdc++ is old.
- `.gitattributes`: forces LF for `*.sh`.
- `tests/integration/steamvr_util.py`: process detection via `/proc`, stopping via signals, launching with `vrstartup.sh`, `~/.steam` / XDG paths.
