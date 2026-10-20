[日本語](../architecture.md) | **English** | [简体中文](../zh-CN/architecture.md) | [한국어](../ko/architecture.md) — [Back to README](../../README.en.md)

# Architecture

## Overview

```
[External clients (Python / any language / VMT sender tools)]
        │ UDP 127.0.0.1:39570  (JSON or VMT-OSC)
        ▼
┌────────────────────── driver_vhmd.dll / .so (inside the vrserver process) ───────────────────────┐
│ UdpReceiver thread ──► MessageRouter ──► DeviceState (per device, mutex-protected)               │
│   net/udp_*                state/router     state/device_state                                   │
│   (only OS-dependent part)  protocol/parser  - latest pose / input / skeleton / gaze             │
│                            protocol/osc     - timeout evaluation, smoothing                      │
│                                                         │                                        │
│ Update thread (default 120Hz, high-resolution timer) ───┘                                        │
│   DeviceProvider::UpdateLoop → VirtualDevice::Update (only changes are sent to SteamVR)          │
│     → TrackedDevicePoseUpdated / UpdateBoolean/Scalar/Skeleton/EyeTrackingComponent              │
│                                                                                                  │
│ RunFrame (vrserver main thread) → on-demand addition, haptic events sent to client over UDP      │
│                                                                                                  │
│ HMD display (one of):                                                                            │
│   Window mode … IVRDisplayComponent. The compositor opens a window on the desktop (Windows)      │
│   Direct mode … IVRDriverDirectModeComponent. The driver receives the left and right eyes        │
│                 rendered by the compositor and also drives vsync (Linux: Vulkan, Windows: D3D11) │
│                 → preview window (Windows) / shared memory (for tools and Python)                │
└──────────────────────────────────────────────────────────────────────────────────────────────────┘
        ▼
     SteamVR (vrserver / vrcompositor) → apps (VRChat, etc.)
```

## Layers

| Layer | Directory | OpenVR dependency | Unit tests |
|---|---|---|---|
| Math | `driver/src/math/` | None | ○ |
| Protocol (JSON / OSC) | `driver/src/protocol/` | None | ○ |
| Config and roles | `driver/src/config/` | None | ○ |
| State and routing | `driver/src/state/` | None | ○ |
| Network | `driver/src/net/` | None (switches between WinSock/BSD internally) | ○ (loopback) |
| Display labels (model and icon generation) | `driver/src/visuals/` | None (bitmap font and PNG writer built in) | ○ |
| Utilities | `driver/src/util/` | None | Partial |
| Devices and provider | `driver/src/*.cpp` | Yes | Integration tests |
| Linux direct mode | `driver/src/linux/` | Yes (+ Vulkan, loaded at runtime) | Capture by self-test, everything by `linux_check.py` |
| Windows direct mode | `driver/src/windows/` | Yes (+ D3D11 / DXGI) | Integration tests (run with `display.mode = direct`) |

The OpenVR-independent parts are bundled into the static library `vhmd_core`, which both the driver DLL and the test executables link against.

## Threads and lifecycle

| Thread | Role | Shutdown |
|---|---|---|
| vrserver main | `Init` / `RunFrame` / `Cleanup`, `Activate` / `Deactivate` | — |
| Receive | `UdpReceiver::Loop`. poll times out every 100 ms to check the stop flag | `Stop()` in `Cleanup` → join (up to ~100 ms) |
| Update | `DeviceProvider::UpdateLoop`. Updates all devices at a fixed rate and measures its own load once per second (`perf` in `status`) | Stop flag in `Cleanup` → join (up to 1 cycle) |
| vsync (direct mode) | `DirectModeComponent::VsyncLoop`. Signals `VsyncEvent` at the `refreshHz` interval (can be changed while running) | Stopped in the HMD's `Deactivate` |
| Capture (Linux) | `FrameCapture::Worker`. Only while frames are requested, downscales the latest frame and writes it to shared memory | Joined when the device is destroyed |
| Preview window (Windows) | `DirectModeComponent::WindowLoop`. Draws the left and right eyes into the window only while it is shown | Stopped in the HMD's `Deactivate` |
| Readback (Windows) | `DirectModeComponent::ReadbackLoop`. Only while frames are requested, reads them back from the GPU and writes them to shared memory | Stopped in the HMD's `Deactivate` |
| Compositor (SteamVR side) | Calls `CreateSwapTextureSet` / `SubmitLayer` / `Present` / `PostPresent`. `PostPresent` is made to wait until the next vsync | — |

- `Init`: load config → create `DeviceState` and device objects for all devices → `TrackedDeviceAdded` only for devices that are **not on demand** (by default, the 3 points: HMD and left/right controllers) → start the receive and update threads.
- On-demand addition: when the receive thread gets a message addressed to an unregistered tracker, it stores the values in the buffer and then calls `RequestAdd(id)` (which only enqueues). `RunFrame` (vrserver main thread) processes the queue in `ProcessPendingAdds()` and calls `TrackedDeviceAdded`. Buffered values are sent starting 100 ms after `Activate`, so the device is in sync immediately after being added.
- Hand mode: each hand has two devices, a "controller" and a "hand" (`VHMD-HAND-L/R`), which share the same `DeviceState` (the hand device is added the first time the mode is set to `hands`). The update thread reads the state only once and passes it to both (input and skeleton data are consumed when read). Only the selected device is reported as connected.
- `Activate(index)`: creates properties and input components, and generates the display labels (model and icon). Mutually exclusive with the update thread via a per-device mutex.
- The update thread does not send poses for a device until 100 ms after `Activate` (sending before vrserver has finished registering device ownership causes a `PoseUpdated ... owned by driver (-1)` error; confirmed by measurement).
- `Deactivate`: invalidates the index → from then on, the update thread does not touch that device.
- `Cleanup`: stop receiving → stop updating → destroy devices → detach the log sink. Confirmed not to hang on SteamVR exit across 31 consecutive start/stop cycles (`docs/verification.md` (Japanese)).
- All exceptions inside the receive handler are caught, and the receive thread keeps running.

## Data flow rules

- The receive side "overwrites with the latest value". There is no queue, so latency does not pile up even with high-rate sending (1000 pps or more).
- Input values are coalesced to the latest value per device and per name, and applied to SteamVR by the update thread.
- Latency breakdown: UDP receipt is immediate; application to SteamVR happens on the next update cycle (up to 8.3 ms at 120 Hz). Measured send → visible-in-app latency is a median of 8.2 ms (`docs/verification.md` (Japanese)).
- **No needless sends**: a pose that has not changed (zero velocity) is only resent every 250 ms, and idle devices (the side not selected in hand mode) once per second. Finger skeletons derived from buttons are computed only when the values change.
- **Frames only when someone is watching**: GPU copies and writes happen for the preview (shared memory) only when it is requested with the `preview` operation, and for the preview window only while the window is shown.
- Socket and shared-image handles are not inherited by child processes (so that a child process cannot keep holding the port if vrserver crashes).

## Extension points

| What you want to do | Where to change |
|---|---|
| New device kind | Derive from `VirtualDevice` (override `OnActivate` for properties/components, `OnInput`/`OnTick`, etc.) and add it to `DeviceKind` and `DeviceProvider::CreateDevices` |
| New message type | Add a struct to `protocol/messages.h` → `ParseOne` in `parser.cpp` → `MessageRouter::HandleDatagram` → `DeviceState` → device hook |
| New `config` operation | Add a branch to `HandleConfig` in `router.cpp`, and pass the driver-side handling as a `RouterHooks` function in `DeviceProvider::Init` |
| New transport (WebSocket, shared memory, etc.) | Receive in the same way as `UdpReceiver` and just call `MessageRouter::HandleDatagram()` (the router is transport-independent) |
| Compatibility with another protocol | Add a parser that returns a `ParseResult`, like `protocol/osc.*`, and add it to the detection in `HandleDatagram` |
| Using the frames (encoding / streaming / recording) | The left and right eyes are received in `Capture()` in `linux/frame_capture.cpp` on Linux and in `CopyPreview()` in `windows/direct_mode.cpp` on Windows. You can add another output in place of shared memory |
| Adding controller inputs | The `InputDef` table in `controller_device.cpp`, `resources/input/vhmd_controller_profile.json`, and if needed the remapping (`tools/gen_remapping.py`) and legacy bindings |
| Look of the display labels | `visuals/device_visuals.cpp` (bitmap font, meshes, icon colors) |

## Platform abstraction

- `util/platform.h`: `VHMD_EXPORT` (`__declspec(dllexport)` / `visibility("default")`), `kPathSeparator`.
- `net/udp_socket.cpp`: switches between WinSock and BSD sockets with `#if`. The header is OS-independent (handles are `intptr_t`).
- `util/precise_sleep.cpp`: on Windows, a high-resolution waitable timer (does not change the system-wide timer via `timeBeginPeriod`); elsewhere, `sleep_until`.
- `util/thread_cpu.cpp`: per-thread CPU time (`GetThreadTimes` / `CLOCK_THREAD_CPUTIME_ID`).
- Direct mode has separate implementations in `linux/` and `windows/` with the same class name and the same public functions; `hmd_device.cpp` includes the one for the current platform.
- CMake: `CMAKE_CXX_VISIBILITY_PRESET hidden`, output to `bin/<win64|linux64|osx64>/driver_vhmd.<dll|so>`, `PREFIX ""`.
- Config file paths are handled as UTF-8 via `std::filesystem::u8path`.
- Linux: for ELF, `driver_exports.map` hides everything except `HmdDriverFactory`, and libstdc++/libgcc are statically linked. Verified on SteamVR for Linux with a real GPU (`docs/linux.md`).

## Names and IDs registered with SteamVR

| Kind | Value |
|---|---|
| Driver name | `vhmd` (`driver.vrdrivermanifest`) |
| Controller types | `vhmd_hmd`, `vhmd_controller`, `vhmd_tracker` (custom; no other vendors' names are used) |
| Manufacturer name | `VirtualHMD (development)` |
| Tracking system name | `vhmd` |
| Tracking universe | `0x56484D44` (shared by all devices). The HMD provides a driver chaperone (seated and standing zero at the origin, 2 × 2 m, bounds hidden) |
| Serials | `VHMD-HMD-0`, `VHMD-CTL-L/R`, `VHMD-HAND-L/R` (hand mode), `VHMD-TRK-0` onward |
| Render models | The HMD uses SteamVR's bundled `generic_hmd`. Controllers and trackers use labeled models that the driver generates at start-up (`resources/rendermodels/vhmd_<serial>_<label>/`, with components such as fingertips). If generation fails, `{vhmd}vhmd_controller` / `generic_tracker` |
| Status icons | Controllers and trackers get icons with a short label (`resources/icons/labels/`, generated at start-up) |
| Registered device type | `vhmd/<serial>` (corresponds to the tracker role setting key `/devices/vhmd/<serial>`) |
