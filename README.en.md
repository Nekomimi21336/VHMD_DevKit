<div align="center">

# VHMD_DevKit

**Virtual HMD / controller / full-body tracker SteamVR driver for development**

No hardware needed: poses and inputs sent from an external process over UDP/JSON appear to SteamVR "as if the real devices were there."

[日本語](README.md) | **English** | [简体中文](README.zh-CN.md) | [한국어](README.ko.md)

</div>

---

## Table of contents

- [Features](#features)
- [Start with 3 points, add only the trackers you need](#start-with-3-points-add-only-the-trackers-you-need)
- [Getting started (prebuilt binaries)](#getting-started-prebuilt-binaries)
- [Building from source](#building-from-source)
- [Usage (Python)](#usage-python)
- [Running without a window / lowering the frame rate (Windows)](#running-without-a-window--lowering-the-frame-rate-windows)
- [Configuration](#configuration)
- [Using alongside a physical HMD](#using-alongside-a-physical-hmd)
- [Tests](#tests)
- [Documentation](#documentation)
- [Repository layout](#repository-layout)
- [Tested environment](#tested-environment)
- [License](#license)

## Features

- **3-point VR is the baseline**: the HMD and left/right controllers are always present from the moment SteamVR starts
- **Trackers on demand**: a tracker is added to SteamVR the first time you send to it, and starts in sync with that pose (up to 16)
- **Simple external API**: UDP + JSON (`127.0.0.1:39570`). A single datagram can update all devices at once. Optional VMT-compatible OSC
- **Full controller input set**: trigger / grip / stick / A, B, X, Y / system / menu, haptics sent back to the client, finger skeleton (curl values or direct bone specification)
- **Switch between controllers and hand tracking**: `hand_mode` changes it while SteamVR runs (in VRChat: button controls ⇄ hand-tracking controls). Ids and messages stay the same
- **Easy to tell apart in SteamVR**: tracker models and status icons show the body part (`L FOOT`, `WAIST`, ...) and the id (`TRACKER_1`); controllers show `L` / `R` and their id. The labels follow role changes
- **Eye gaze input** (SteamVR's eye tracking component)
- **Windows / Linux** support. On Linux the driver runs in direct mode (the driver receives the frames SteamVR renders), and it has been verified on SteamVR for Linux with a real GPU
- **Runs windowless at low load**: direct mode is available on Windows too, with a choice of whether to show a preview window and of the frame rate (1-240 Hz, changeable while running). Frames can be grabbed from Python (for machine learning and automation)
- **Python client** (no dependencies) with samples, plus automated verification scripts that run against SteamVR
- **No impersonation**: registers its own controller types `vhmd_*`. Existing apps' bindings are reused via SteamVR remapping

> Intended uses: developing and testing VR apps, verifying avatar / IK / full-body tracking behavior, and feeding in motion-capture data.
> Do not use it for cheating, impersonating other people's devices, or abuse in multiplayer environments.

## Start with 3 points, add only the trackers you need

| Setup | Devices to send | Example use |
|---|---|---|
| **3-point** | `hmd`, `left_hand`, `right_hand` | Regular VR |
| **5-point** | + `left_foot`, `right_foot` (ankles) | Foot movement |
| **6-point** | + `waist` | Full-body tracking in VRChat |
| 11-point | + knees, elbows, chest (all 8 default trackers) | Detailed motion capture |
| Up to 19-point | Add trackers in `vhmd_config.json` (up to 16; shoulders, wrists, ankles, etc. can also be specified) | |

A tracker is **added the moment you send to it** (about 40 ms measured), and shows as disconnected once sending stops. The registration persists until SteamVR is restarted; sending again brings it back.

## Getting started (prebuilt binaries)

1. Download the archive for your OS from [Releases](../../releases) and extract it to a **location you won't move** (the extracted folder itself is registered with SteamVR).
2. Register it from the extracted folder (uses SteamVR's `vrpathreg`; nothing is copied into SteamVR itself).

   | OS | Archive | Registration command |
   |---|---|---|
   | Windows | `VHMD_DevKit-<version>-win64.zip` | `powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1` |
   | Linux | `VHMD_DevKit-<version>-linux64.tar.gz` | `tools/install_driver.sh` (glibc 2.38 or later) |

3. Start SteamVR and run a sample.

   ```bash
   python clients/python/examples/fullbody_sway.py --points 6
   ```

   `--points` is `3` / `5` / `6` / `11`.

4. To unregister, use `tools/uninstall_driver.ps1` (`.sh` on Linux). Adding `-CleanSettings` (`--clean-settings` on Linux) also removes the tracker roles the driver wrote into the SteamVR settings, with a backup.

## Building from source

```bash
git clone --recurse-submodules https://github.com/Nekomimi21336/VHMD_DevKit.git
cd VHMD_DevKit
```

### Windows

Prerequisites: Visual Studio 2022 (Desktop development with C++), CMake 3.20 or later, Git, Steam + SteamVR, Python 3.10 or later.

```bash
cmake --preset vs2022
cmake --build --preset vs2022-release
ctest --preset vs2022-release
powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1
```

- The output is `build/driver/vhmd/`, which SteamVR can load directly.
- **Quit SteamVR before rebuilding** (a DLL that is loaded cannot be overwritten).
- If Visual Studio 2022 is the only version installed, `cmake -S . -B build -A x64` → `cmake --build build --config Release` also works. If a newer Visual Studio is installed without the C++ workload, CMake picks it and fails, so the presets specify VS 2022.
- If `third_party/openvr` (submodule) is missing, OpenVR SDK v2.15.6 is fetched automatically at configure time.

### Linux

Prerequisites: g++ 9 or later, CMake 3.20 or later, Git, Python 3.10 or later, SteamVR for Linux.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
tools/install_driver.sh
```

- SteamVR for Linux cannot start in "draw into a desktop window" mode, so on Linux the driver runs in direct mode, receiving SteamVR's rendered output directly. You can view the rendered frames with `python3 clients/python/examples/preview_viewer.py`.
- To verify on real hardware, run `python3 tests/integration/linux_check.py` (automates everything from starting SteamVR to shutting it down, and writes a report).

For details, see [docs/en/linux.md](docs/en/linux.md).

## Usage (Python)

```python
import sys; sys.path.insert(0, "clients/python")   # or: pip install -e clients/python
from vhmd_client import VHMDClient, math3d

with VHMDClient() as c:                            # 127.0.0.1:39570
    with c.batch():                                # bundled into 1 datagram
        c.pose("hmd", (0, 1.7, 0), math3d.from_euler(yaw=30))
        c.pose("left_hand", (-0.2, 1.2, -0.3))
        c.pose("right_hand", (0.2, 1.2, -0.3))
        c.pose("left_foot", (-0.12, 0.08, 0))      # added to SteamVR the first time it is sent
        c.pose("right_foot", (0.12, 0.08, 0))
    c.inputs("right_hand", {"trigger": 1.0, "a": True, "joystick_x": 0.5})
    c.skeleton_curl("left_hand", [0, 1, 1, 1, 1])  # close all fingers except the thumb
    c.gaze(direction=(0.1, 0, -1))
    print(c.status())                              # registration state, receive rate, last receive time
```

From other languages, just send JSON over UDP.

```json
{ "type": "pose", "id": "left_foot", "pos": [-0.12, 0.08, 0.0], "rot": [1, 0, 0, 0] }
```

| `type` | Contents |
|---|---|
| `pose` | Position, rotation, velocity |
| `input` | Buttons, triggers, sticks |
| `skeleton` | Fingers (curl values or 31 bones) |
| `gaze` | Eye gaze |
| `config` | `list` / `ping` / `add` / `set_role` / `reset` |

- The coordinate system follows SteamVR (right-handed, +Y up, -Z forward, meters); rotations are `[w, x, y, z]`.
- Invalid values (NaN, out of range, wrong type, etc.) are discarded, and the driver keeps running.
- Full specification: [docs/en/protocol.md](docs/en/protocol.md)

## Running without a window / lowering the frame rate (Windows)

Changing `display` in `vhmd_config.json` switches to **direct mode**: SteamVR opens no desktop window and the driver receives the images itself (the same approach as on Linux). Like a camera library, you choose whether a preview window is shown, and you can grab the frames from Python.

```jsonc
"display": { "mode": "direct", "refreshHz": 30, "previewWindow": true }
```

| Setting | Meaning |
|---|---|
| `mode: "direct"` | Direct mode (the default `auto` is window mode on Windows) |
| `refreshHz` | Frame rate of the virtual display (1-240, default 90). 30 or 15 is plenty for machine learning or automation and lowers the load of SteamVR and the app. Can be changed while running |
| `previewWindow` | Show the preview window (both eyes) at start. Closing it does not stop SteamVR |
| `preview.windowFps` | Refresh rate of the preview window (default 30) |

Switching at runtime (`VHMDClient`):

```python
c.refresh_rate(30)        # frame rate 30 (without restarting SteamVR; no argument returns the current rate)
c.preview_window(True)    # show the preview window / False hides it
c.preview(True)           # write frames to shared memory (called by the reader; False stops)
from vhmd_client.preview import PreviewReader
frame = PreviewReader().read()   # both eyes side by side, RGBA; frame.save_png("eyes.png") etc.
```

The SteamVR compositor follows any value from 1 to 240 Hz exactly. Below 10 Hz, however, apps do not drop under 12-15 fps (SteamVR stops waiting), so about 15 Hz is the practical floor for reducing an app's load.

`python clients/python/examples/preview_viewer.py` shows the image as well. While nobody is looking (window hidden, no frames requested), no image is copied at all.

Load comparison (Windows 11, RX 7900 XTX, a test app that renders every frame, CPU as % of one core):

| Case | App fps | CPU: compositor + vrserver + app | GPU (3D): compositor + vrserver + app |
|---|---|---|---|
| Window mode 90 Hz (before) | 66.6 | 32.1 + 5.1 + 1.6 = **38.8 %** | n/a + 0 + 3.7 % |
| Direct 90 Hz (no window) | 89.9 | 10.0 + 6.9 + 1.5 = **18.4 %** | 6.8 + 0 + 2.7 = 9.5 % |
| Direct 90 Hz + preview window | 89.9 | 10.7 + 8.0 + 2.0 = 20.7 % | 7.1 + 1.5 + 2.5 = 11.1 % |
| Direct 90 Hz + frames to Python at 15 fps | 89.9 | 11.2 + 6.6 + 1.8 = 19.6 % | 6.8 + 0.6 + 2.6 = 10.0 % |
| Direct 60 Hz | 59.9 | 7.2 + 7.8 + 1.1 = 16.1 % | 4.6 + 0 + 1.8 = 6.4 % |
| Direct 30 Hz | 30.0 | 5.5 + 7.2 + 0.0 = **12.7 %** | 2.8 + 0 + 0.9 = **3.7 %** |
| Direct 15 Hz | 15.0 | 3.1 + 6.0 + 0.3 = **9.4 %** | 1.3 + 0 + 0.6 = **1.9 %** |

Window mode tops out at 66 fps for the app and its compositor uses more CPU. Direct mode reaches 90 fps at about half the total load, and 30 Hz cuts it to roughly a third (the heavier the app, the more lowering the frame rate saves). Details: [docs/verification.md](docs/verification.md) (Japanese)

## Configuration

| File | Contents |
|---|---|
| `driver/resources/settings/vhmd_config.json` | Network, update rate, timeouts (`hold` / `invalid` / `disconnect`), smoothing, VMT compatibility, per-device enablement / role / default pose, `trackersOnDemand` |
| `driver/resources/settings/default.vrsettings` | `driver_vhmd` (enablement, `configFile` to specify an external config file), `vhmd_display` (window / resolution / FOV / refresh rate / IPD) |

Settings are loaded when SteamVR starts. Either edit the source-side files and rebuild, or point `configFile` to an external file.

**Security**: by default the driver listens only on `127.0.0.1`. To send from another PC, change `network.bind` and explicitly set `"allowRemote": true`. There is no authentication, so use it only on trusted networks and configure your firewall yourself.

## Using alongside a physical HMD

- This driver's HMD is always registered, so if it is enabled at the same time as a real HMD's driver, which one becomes active depends on your environment.
- Before playing on real hardware, either unregister the driver or set `"hmd": {"enabled": false}` in `vhmd_config.json` to switch to **a mode that only adds controllers / trackers** (not yet tested with a real HMD connected).
- You can also disable `vhmd` in SteamVR under "Startup/Shutdown → Manage Add-ons".

## Tests

| Kind | Command | Contents |
|---|---|---|
| C++ unit tests | `ctest --preset vs2022-release` | 45 tests (parser, coordinate conversion, config, timeouts, router, UDP, label generation) |
| Python unit tests | `python -m pytest tests/python` | 14 tests (client, preview reader) |
| Linux capture self-test | `ctest` (Linux) | Checks shared image → shared memory with real Vulkan (no SteamVR needed) |
| Linux on-hardware check | `python3 tests/integration/linux_check.py` | Direct-mode display path + integration tests, writes a report |
| SteamVR integration tests | `python tests/integration/run_integration.py` | 31 items (enumeration, on-demand addition, error, latency, input, skeleton, hand mode switching, invalid packets, load, shutdown) |
| Repeated start/stop | `python tests/integration/stress_restart.py --cycles 30` | Checks the driver's shutdown and whether it crashed on every cycle |
| Long-run stability | `python tests/integration/soak_test.py --minutes 30` | Records frame rate, pose error, memory growth and warnings at regular intervals |

The integration tests require `pip install openvr`. In environments with an overlay app that relaunches SteamVR after it starts, add `--stop-relaunchers` (Steam itself and games are not affected).

## Documentation

| Contents | 日本語 | English | 简体中文 | 한국어 |
|---|---|---|---|---|
| README | [README.md](README.md) | This page | [README.zh-CN.md](README.zh-CN.md) | [README.ko.md](README.ko.md) |
| Protocol specification | [protocol](docs/protocol.md) | [protocol](docs/en/protocol.md) | [protocol](docs/zh-CN/protocol.md) | [protocol](docs/ko/protocol.md) |
| Architecture and extension | [architecture](docs/architecture.md) | [architecture](docs/en/architecture.md) | [architecture](docs/zh-CN/architecture.md) | [architecture](docs/ko/architecture.md) |
| Linux | [linux](docs/linux.md) | [linux](docs/en/linux.md) | [linux](docs/zh-CN/linux.md) | [linux](docs/ko/linux.md) |
| Verification procedures and results | [verification](docs/verification.md) | — | — | — |
| Record of decisions and open items | [decisions](docs/decisions.md) | — | — | — |
| Changelog | [CHANGELOG](CHANGELOG.md) | — | — | — |

The verification records, decision records and changelog are development logs, so they are available in Japanese only.

## Repository layout

```
CMakeLists.txt / CMakePresets.json   Build configuration (fetching the OpenVR SDK, per-platform output)
driver/
  driver.vrdrivermanifest
  resources/            Settings, input profiles, remapping, legacy bindings, icons, controller models
  src/
    main.cpp            HmdDriverFactory (the only exported symbol)
    device_provider.*   Device management, receive / update threads, on-demand addition
    device_base.*       Common to all devices
    hmd_device.*        HMD, virtual display, eye gaze
    linux/              Linux direct mode, frame capture, dynamic loading of Vulkan
    windows/            Windows direct mode (D3D11), preview window, shared memory
    visuals/            Generation of the labels (models, icons) shown in SteamVR
    controller_device.* Controllers, haptics, skeleton
    tracker_device.*    Trackers, roles
    math/ protocol/ config/ state/ net/ util/   OpenVR-independent core (covered by unit tests)
clients/python/         vhmd_client package and examples/
tools/                  Register / unregister / environment check (Windows: .ps1, Linux: .sh), packaging, model generation
tests/                  unit/ (C++), python/, integration/ (SteamVR)
docs/                   Documentation (table above)
third_party/            OpenVR SDK (submodule v2.15.6), nlohmann/json 3.11.3
```

## Tested environment

| Item | Version |
|---|---|
| SteamVR | 2.17.10 (Windows 11; Linux: Ubuntu 26.04 + NVIDIA RTX 4000 Ada) |
| OpenVR SDK | v2.15.6 |
| Windows compiler | MSVC 14.44 (Visual Studio 2022 Build Tools 17.14), CMake 4.3 |
| Linux compiler | gcc 13.3 (Ubuntu 24.04 / WSL2), gcc 15.2 (Ubuntu 26.04), CMake 3.28 / 4.2 |
| Python | 3.12 (3.10 or later), pyopenvr 2.12 for verification |

Detailed results: [docs/verification.md](docs/verification.md) (Japanese). On SteamVR for Linux, v0.3.0's direct mode was verified on a real GPU (NVIDIA RTX 4000 Ada): the compositor renders at 90 fps, the integration test passes 28/28, SteamVR shuts down cleanly with the Steam client and SteamVR Home, and VRChat through Proton renders and follows the controllers. On a GTX 1050 the SteamVR compositor stops on a GPU exception shortly after start ([docs/en/linux.md](docs/en/linux.md)). While logged in to VRChat, avatar tracking, fingers, locomotion, menu operation and full-body calibration were confirmed on Windows (controllers) and Linux (hand mode). Use alongside a real HMD, a Linux desktop with a physical display, and starting/stopping the preview on Linux (after the load reduction) have not been verified. Changelog: [CHANGELOG.md](CHANGELOG.md) (Japanese)

## License

**BSD 3-Clause License** — Copyright (c) 2026, Nekomimi21336 ([LICENSE](LICENSE))

| | |
|---|---|
| ✅ Commercial use | Allowed |
| ✅ Modification and derivative works | Allowed |
| ✅ Redistribution (source / prebuilt binaries) | Allowed |
| 📄 **Obligation** | When redistributing, include the copyright notice, conditions, and disclaimer (the full text of [LICENSE](LICENSE)): as-is for source, and in the accompanying documentation etc. for binaries |
| 🚫 Prohibited | Using the copyright holder's name to endorse or promote derivative works without permission |
| ⚠ No warranty | Provided as is; the copyright holder is not liable for any damages arising from its use |

The summary above is for reference only. The English text of [LICENSE](LICENSE) is authoritative and governs the formal terms.

Example credit notice (in documentation, an About screen, etc.):

```
This product includes VHMD_DevKit
Copyright (c) 2026, Nekomimi21336 — BSD 3-Clause License
https://github.com/Nekomimi21336/VHMD_DevKit
```

The licenses of the bundled third-party components (OpenVR SDK: BSD 3-Clause / nlohmann/json: MIT) are in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Include it as well when redistributing (it is already included in the release archives).
