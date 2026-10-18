[日本語](../architecture.md) | **English** | [简体中文](../zh-CN/architecture.md) | [한국어](../ko/architecture.md) — [Back to README](../../README.en.md)

# Architecture

## Overview

```
[External clients (Python / any language / VMT sender tools)]
        │ UDP 127.0.0.1:39570  (JSON or VMT-OSC)
        ▼
┌───────────────────────── driver_vhmd.dll (inside the vrserver process) ──────────────────┐
│ UdpReceiver thread ──► MessageRouter ──► DeviceState (per device, mutex-protected)        │
│   net/udp_*              state/router       state/device_state                            │
│   (only OS-dependent part) protocol/parser  - latest pose / input / skeleton / gaze       │
│                          protocol/osc       - timeout evaluation, smoothing               │
│                                                    │                                     │
│ Update thread (default 120Hz, high-resolution timer) ┘                                    │
│   DeviceProvider::UpdateLoop → VirtualDevice::Update                                      │
│     → TrackedDevicePoseUpdated / UpdateBoolean/Scalar/Skeleton/EyeTrackingComponent       │
│                                                                                           │
│ RunFrame (vrserver main thread) → PollNextEvent → haptic events sent to client over UDP   │
└───────────────────────────────────────────────────────────────────────────────────────────┘
        ▼
     SteamVR (vrserver) → apps (VRChat, etc.)
```

## Layers

| Layer | Directory | OpenVR dependency | Unit tests |
|---|---|---|---|
| Math | `driver/src/math/` | None | ○ |
| Protocol (JSON / OSC) | `driver/src/protocol/` | None | ○ |
| Config and roles | `driver/src/config/` | None | ○ |
| State and routing | `driver/src/state/` | None | ○ |
| Network | `driver/src/net/` | None (switches between WinSock/BSD internally) | ○ (loopback) |
| Utilities | `driver/src/util/` | None | Partial |
| Devices and provider | `driver/src/*.cpp` | Yes | Integration tests |
| Linux direct mode | `driver/src/linux/` | Yes (+ Vulkan, loaded at runtime) | Capture by self-test, everything by `linux_check.py` |

The OpenVR-independent parts are bundled into the static library `vhmd_core`, which both the driver DLL and the test executables link against.

## Threads and lifecycle

| Thread | Role | Shutdown |
|---|---|---|
| vrserver main | `Init` / `RunFrame` / `Cleanup`, `Activate` / `Deactivate` | — |
| Receive | `UdpReceiver::Loop`. poll times out every 100 ms to check the stop flag | `Stop()` in `Cleanup` → join (up to ~100 ms) |
| Update | `DeviceProvider::UpdateLoop`. Updates all devices at a fixed rate | Stop flag in `Cleanup` → join (up to 1 cycle) |
| vsync (Linux) | `DirectModeComponent::VsyncLoop`. Signals `VsyncEvent` at the display refresh rate | Stopped in the HMD's `Deactivate` |
| Capture (Linux) | `FrameCapture::Worker`. Takes only the latest frame, downscales it and writes it to shared memory | Joined when the device is destroyed |
| Compositor (SteamVR side) | Calls `CreateSwapTextureSet` / `SubmitLayer` / `Present` / `PostPresent`. `PostPresent` is made to wait until the next vsync | — |

- `Init`: load config → create `DeviceState` and device objects for all devices → `TrackedDeviceAdded` only for devices that are **not on demand** (by default, the 3 points: HMD and left/right controllers) → start the receive and update threads.
- On-demand addition: when the receive thread gets a message addressed to an unregistered tracker, it stores the values in the buffer and then calls `RequestAdd(id)` (which only enqueues). `RunFrame` (vrserver main thread) processes the queue in `ProcessPendingAdds()` and calls `TrackedDeviceAdded`. Buffered values are sent starting 100 ms after `Activate`, so the device is in sync immediately after being added.
- `Activate(index)`: creates properties and input components. Mutually exclusive with the update thread via a per-device mutex.
- The update thread does not send poses for a device until 100 ms after `Activate` (sending before vrserver has finished registering device ownership causes a `PoseUpdated ... owned by driver (-1)` error; confirmed by measurement).
- `Deactivate`: invalidates the index → from then on, the update thread does not touch that device.
- `Cleanup`: stop receiving → stop updating → destroy devices → detach the log sink. Confirmed not to hang on SteamVR exit across 31 consecutive start/stop cycles (`docs/verification.md` (Japanese)).
- All exceptions inside the receive handler are caught, and the receive thread keeps running.

## Data flow rules

- The receive side "overwrites with the latest value". There is no queue, so latency does not pile up even with high-rate sending (1000 pps or more).
- Input values are coalesced to the latest value per device and per name, and applied to SteamVR by the update thread.
- Latency breakdown: UDP receipt is immediate; application to SteamVR happens on the next update cycle (up to 8.3 ms at 120 Hz). Measured send → visible-in-app latency is a median of 8.2 ms (`docs/verification.md` (Japanese)).
- Poses are sent to `TrackedDevicePoseUpdated` every cycle (SteamVR considers devices whose updates have stopped to be stale).

## Extension points

| What you want to do | Where to change |
|---|---|
| New device kind | Derive from `VirtualDevice` (override `OnActivate` for properties/components, `OnInput`/`OnTick`, etc.) and add it to `DeviceKind` and `DeviceProvider::CreateDevices` |
| New message type | Add a struct to `protocol/messages.h` → `ParseOne` in `parser.cpp` → `MessageRouter::HandleDatagram` → `DeviceState` → device hook |
| New transport (WebSocket, shared memory, etc.) | Receive in the same way as `UdpReceiver` and just call `MessageRouter::HandleDatagram()` (the router is transport-independent) |
| Compatibility with another protocol | Add a parser that returns a `ParseResult`, like `protocol/osc.*`, and add it to the detection in `HandleDatagram` |
| Using the frames (encoding / streaming / recording) | Linux: `Capture()` in `linux/frame_capture.cpp` is where the left and right eyes are received. You can add another output in place of shared memory. To use the same approach on Windows, implement `IVRDriverDirectModeComponent` with D3D11 shared textures |
| Adding controller inputs | The `InputDef` table in `controller_device.cpp`, `resources/input/vhmd_controller_profile.json`, and if needed the remapping (`tools/gen_remapping.py`) and legacy bindings |

## Platform abstraction (future Linux support)

- `util/platform.h`: `VHMD_EXPORT` (`__declspec(dllexport)` / `visibility("default")`), `kPathSeparator`.
- `net/udp_socket.cpp`: switches between WinSock and BSD sockets with `#if`. The header is OS-independent (handles are `intptr_t`).
- `util/precise_sleep.cpp`: on Windows, a high-resolution waitable timer (does not change the system-wide timer via `timeBeginPeriod`); elsewhere, `sleep_until`.
- CMake: `CMAKE_CXX_VISIBILITY_PRESET hidden`, output to `bin/<win64|linux64|osx64>/driver_vhmd.<dll|so>`, `PREFIX ""`.
- Config file paths are handled as UTF-8 via `std::filesystem::u8path`.
- Linux: built and unit-tested on WSL2. For ELF, `driver_exports.map` hides everything except `HmdDriverFactory`, and libstdc++/libgcc are statically linked. Operation on SteamVR for Linux is unverified (`docs/linux.md`).

## Names and IDs registered with SteamVR

| Kind | Value |
|---|---|
| Driver name | `vhmd` (`driver.vrdrivermanifest`) |
| Controller types | `vhmd_hmd`, `vhmd_controller`, `vhmd_tracker` (custom; no other vendors' names are used) |
| Manufacturer name | `VirtualHMD (development)` |
| Tracking system name | `vhmd` |
| Render models | SteamVR's bundled generic models `generic_hmd` / `generic_controller` / `generic_tracker` |
| Registered device type | `vhmd/<serial>` (corresponds to the tracker role setting key `/devices/vhmd/<serial>`) |
