[日本語](../protocol.md) | **English** | [简体中文](../zh-CN/protocol.md) | [한국어](../ko/protocol.md) — [Back to README](../../README.en.md)

# External input protocol (UDP / JSON, VMT-compatible OSC)

The `vhmd` driver writes messages received over UDP into each virtual device's "latest-value buffer", and an update thread (120 Hz by default) applies them to SteamVR.

| Item | Value |
|---|---|
| Transport | UDP (IPv4) |
| Default address | `127.0.0.1:39570` (`network.bind` / `network.port` in `vhmd_config.json`) |
| External exposure | Binding to addresses other than `127.0.0.1` is allowed only when `network.allowRemote: true` (not allowed by default) |
| Format | 1 datagram = one JSON object, or a JSON array (batch, up to 256 entries) |
| Maximum size | 65507 bytes (UDP limit) |
| Encoding | UTF-8 |
| VMT-compatible OSC | When `vmt.enabled: true`, received on the same port (treated as OSC if the first byte is `/` or `#`) |

## Coordinate system and units

- **Follows SteamVR**: right-handed, **+Y up**, **-Z forward**, +X right, units in meters.
- Rotations are always **quaternions `[w, x, y, z]`** (normalized on receipt even if not normalized).
- Velocity `vel` is in m/s and angular velocity `angvel` in rad/s (both in world coordinates).
- `pos` is "the position of the head (HMD)". If you want it treated as the neck position with SteamVR's neck model applied, set `hmd.headModel: true`.
- Poses go into `TrackingUniverseRawAndUncalibrated` as is (room setup offsets are applied by SteamVR).

## Device IDs

| id | Kind | SteamVR class / role | Default serial |
|---|---|---|---|
| `hmd` | HMD | `TrackedDeviceClass_HMD` | `VHMD-HMD-0` |
| `left_hand` | Controller | `Controller` / `TrackedControllerRole_LeftHand` | `VHMD-CTL-L` |
| `right_hand` | Controller | `Controller` / `TrackedControllerRole_RightHand` | `VHMD-CTL-R` |
| `tracker_0` | Tracker | `GenericTracker` / `TrackerRole_Waist` | `VHMD-TRK-0` |
| `tracker_1` | 〃 | `TrackerRole_LeftFoot` | `VHMD-TRK-1` |
| `tracker_2` | 〃 | `TrackerRole_RightFoot` | `VHMD-TRK-2` |
| `tracker_3` | 〃 | `TrackerRole_LeftKnee` | `VHMD-TRK-3` |
| `tracker_4` | 〃 | `TrackerRole_RightKnee` | `VHMD-TRK-4` |
| `tracker_5` | 〃 | `TrackerRole_LeftElbow` | `VHMD-TRK-5` |
| `tracker_6` | 〃 | `TrackerRole_RightElbow` | `VHMD-TRK-6` |
| `tracker_7` | 〃 | `TrackerRole_Chest` | `VHMD-TRK-7` |

- **The baseline is 3 points** (`hmd`, `left_hand`, `right_hand`). Only these 3 devices are registered when SteamVR starts.
- **Trackers are on demand**: a tracker is added to SteamVR the first time a message addressed to it (`pose`, etc.) arrives, and starts in sync with the received pose (about 40 ms measured). Send both feet for 5 points; add the waist for 6 points. You can also add trackers in advance with `config` `op: "add"`. Added devices remain until SteamVR exits (OpenVR has no device-removal API), and become `disconnect` (shown grayed out) when sending stops. To register all devices at startup, set `"trackersOnDemand": false`.
- Up to 16 trackers can be configured. The `id` and role can be changed in the `trackers` array of `vhmd_config.json`.
- **Role names can also be used**: if only one tracker has a given role, you can send with a short role name such as `"id": "waist"` (equivalent to `tracker_0`).
- Role names are accepted in 3 forms: `waist` / `vive_tracker_waist` / `TrackerRole_Waist`.
  List: `handed, left_foot, right_foot, left_shoulder, right_shoulder, left_elbow, right_elbow, left_knee, right_knee, left_wrist, right_wrist, left_ankle, right_ankle, waist, chest, camera, keyboard` (those present in the profiles bundled with SteamVR 2.17.10).

## Messages

### `pose`

```json
{ "type": "pose", "id": "hmd",
  "pos": [0.0, 1.7, 0.0], "rot": [1.0, 0.0, 0.0, 0.0],
  "vel": [0, 0, 0], "angvel": [0, 0, 0], "ts": 1760000000.123 }
```

| Field | Required | Description |
|---|---|---|
| `pos` | ○ | [x, y, z] m. Absolute value of each component ≤ 1000 |
| `rot` | ○ | [w, x, y, z]. Zero norm / NaN is invalid |
| `vel`, `angvel` | | [x, y, z]. Absolute value ≤ 100. Used for SteamVR's prediction (0 if absent) |
| `acc`, `angacc` | | Acceleration [x, y, z] (m/s², rad/s²). Absolute value ≤ 1000. Passed to SteamVR as is |
| `age` | | How many seconds old the sample already is when sent (0 to 0.5). SteamVR predicts from the time the sample was taken (`poseTimeOffset`). For senders with latency, such as camera-based estimation |
| `ts` | | Sender's UNIX time (seconds). For diagnostics (`latency_ms` in `status`) |
| `src` | | Sender name (up to 32 characters), on `pose` / `input` / `skeleton` / `gaze`. Only the sender holding control (`config` op `control`) is applied |
| `valid` | | `false` explicitly signals "tracking lost". `pos`/`rot` may be omitted. Recovers on the next valid `pose` |

### `input` (controllers / trackers / HMD)

```json
{ "type": "input", "id": "right_hand", "name": "trigger", "value": 0.8 }
{ "type": "input", "id": "right_hand", "values": { "trigger": 0.8, "a": true, "joystick_x": -0.3 } }
```

Values are numbers (-1 to 1) or booleans. A button counts as pressed at 0.5 or above.

| Name | SteamVR input path | Type / range |
|---|---|---|
| `trigger` | `/input/trigger/value` | 0 to 1 |
| `trigger_click` / `trigger_touch` | `/input/trigger/click` / `touch` | bool |
| `grip` | `/input/grip/value` | 0 to 1 |
| `grip_click` / `grip_touch` | `/input/grip/click` / `touch` | bool |
| `joystick_x`, `joystick_y` | `/input/joystick/x`, `y` | -1 to 1 (up is +y) |
| `joystick_click` / `joystick_touch` | `/input/joystick/click` / `touch` | bool |
| `a`, `b` (right hand) / `x`, `y` (left hand) | `/input/a/click` etc. | bool |
| `a_touch`, `b_touch`, `x_touch`, `y_touch` | `/input/a/touch` etc. | bool |
| `system`, `system_touch` | `/input/system/click` / `touch` | bool |
| `menu` | `/input/menu/click` | bool |
| `trackpad_x`, `trackpad_y` | `/input/trackpad/x`, `y` | -1 to 1 |
| `trackpad_click` / `trackpad_touch` | `/input/trackpad/click` / `touch` | bool |
| `trackpad_force` | `/input/trackpad/force` | 0 to 1 |
| `grip_force` | `/input/grip/force` | 0 to 1 |
| `thumbrest_touch` | `/input/thumbrest/touch` | bool |

Trackers (equivalent to the POGO pin inputs of real trackers):

| Name | SteamVR input path | Type / range |
|---|---|---|
| `system`, `menu`, `grip` | `/input/system/click`, `/input/menu/click`, `/input/grip/click` | bool |
| `trigger` / `trigger_click` | `/input/trigger/value` / `click` | 0 to 1 / bool |
| `trackpad_x`, `trackpad_y`, `trackpad_click`, `trackpad_touch` | `/input/trackpad/...` | -1 to 1 / bool |

HMD:

| Name | SteamVR input path | Type / range |
|---|---|---|
| `system` | `/input/system/click` | bool |
| `proximity` | `/proximity` | bool. Proximity (wear) sensor (true = being worn; the default is the `hmd.proximity` setting). Reaches apps as `k_EButton_ProximitySensor`, `/user/head/proximity`, and OpenXR's `XR_EXT_user_presence` |

All devices:

| Name | Description |
|---|---|
| `battery` | Battery level 0 to 1 (`Prop_DeviceBatteryPercentage_Float`). Controllers and trackers report 1.0 (fully charged) until a value is sent; the HMD becomes battery-powered once a value is sent |
| `charging` | Charging (`Prop_DeviceIsCharging_Bool`) |

- Aliases: `primary` = right-hand `a` / left-hand `x`, `secondary` = right-hand `b` / left-hand `y`. Sending `a`/`b` to the left hand is treated as `x`/`y`, and sending `x`/`y` to the right hand as `a`/`b`. `thumbstick_*`, `stick_*` = `joystick_*`, `squeeze` = `grip`, `application_menu` = `menu`, `touchpad_*` = `trackpad_*`.
- Raw paths (e.g. `"/input/y/click"`) can also be used as is.
- If only `trigger` / `grip` is sent, `*_click` (ON at 0.75 / OFF at 0.65) and `*_touch` (0.05 / 0.02) are generated automatically. Once `*_click` / `*_touch` is sent explicitly even once, auto-generation stops for that item.
- The trackpad and grip have force in the input profile, so SteamVR derives clicks from the force value. Until the client sends `*_force`, `*_click` is also mirrored to force (sending only the click still registers a press).
- Values arriving within the same update cycle are coalesced to the latest value per name.

### `skeleton` (fingers of the left/right controllers)

```json
{ "type": "skeleton", "id": "left_hand", "curl": [0.0, 1.0, 1.0, 1.0, 1.0], "splay": [0, 0, 0, 0, 0] }
{ "type": "skeleton", "id": "left_hand", "bones": [[w,x,y,z], ... 31 entries], "positions": [[x,y,z], ... 31 entries] }
```

- `curl`: curl of thumb, index, middle, ring, little finger, from 0 (open) to 1 (closed). `splay`: -1 to 1 (0 if omitted). Bones are generated using the hand model from the OpenVR sample `handskeletonsimulation`.
- `bones`: rotations in **parent-bone space** for the OpenVR hand skeleton (31 bones, in `HandSkeletonBone` order). Positions can be given at the same time as `[w,x,y,z,px,py,pz]`, or passed separately in `positions` (if omitted, the reference positions of an open hand are used).
- While nothing arrives for 500 ms or more after the last `skeleton`, finger curl is generated automatically from `trigger`/`grip`/button touch states.
- The same values are sent to SteamVR for both the `WithController` and `WithoutController` motion ranges.
- Controllers and hand tracking can be **switched while SteamVR runs** (`config` op `hand_mode`). `controllers` (default) behave like hardware controllers with a Partial skeleton (SteamVR tidies the finger bones). `hands` swaps each hand to a separate device (`VHMD-HAND-L/R`, Full skeleton: bones are passed through as sent), which apps such as VRChat treat as hand tracking (button-driven menus are disabled there; gestures are used instead). The ids stay `left_hand` / `right_hand` in both modes, so senders need no changes. The hand devices are added the first time `hands` is selected. VRChat menus can be used with the laser + trigger in either mode. When aiming the laser programmatically, point it at the menu face from close to straight on (at shallow angles it does not register a hit).

### `gaze` (HMD eye gaze)

```json
{ "type": "gaze", "id": "hmd", "origin": [0, 0, 0], "dir": [0, 0, -1] }
{ "type": "gaze", "id": "hmd", "origin": [0, 0, 0], "target": [0.1, 0.0, -1.0] }
{ "type": "gaze", "id": "hmd", "valid": false }
```

Passed to `IVRDriverInput::CreateEyeTrackingComponent("/eyetracking")` as `VREyeTrackingData_t` (origin, target).

- **Coordinates are in HMD-local space** (meters, right-handed, −Z forward). `origin` is the eye position and `target` is the fixation point (a point, not a direction; sending `dir` makes `origin + dir` the fixation point). SteamVR applies the HMD pose before apps see it (measured).
- Reading it in apps: OpenVR uses `IVRInput::GetEyeTrackingDataRelativeToNow` / `GetEyeTrackingDataForNextFrame` (an `eyetracking` action bound to `/user/head/eyetracking`). OpenXR uses `XR_EXT_eye_gaze_interaction` (`/user/eyes_ext/input/gaze_ext/pose`). Both are covered by the integration tests.
- With `valid: false`, OpenVR reports `bValid` / `bTracked` false and OpenXR clears the location flags.
- It is a single combined gaze ray (an OpenVR limitation: no per-eye data or eyelid openness).

### `config` (request / response)

```json
{ "type": "config", "op": "list", "seq": 1 }
```

`seq` is echoed back in the response. Responses are sent back over UDP to the sender's address/port.

| op | Arguments | Response |
|---|---|---|
| `list` (`status`) | | `{"type":"status", "devices":[...], "packets":{"total","rejected"}, ...}` |
| `ping` | | `{"type":"result","op":"ping","ok":true,"version":"0.1.0"}` |
| `roles` | | List of available roles |
| `set_role` | `id`, `role` | Writes the role into the `trackers` section of the SteamVR settings (overwrites the user's setting) |
| `reset` | `id` (all devices if omitted) | Restores the default pose from the config file |
| `add` | `id` (all on-demand devices if omitted) | Adds on-demand devices to SteamVR immediately (applied on the next frame). `{"ok":true,"ids":[...]}` |
| `control` | `source` (a sender name, `""` = anyone; without it only reports), `devices` (default: all) | Chooses which sender drives the devices (SteamVR keeps running). Messages from other senders are dropped and counted. A device changing hands has the inputs the previous sender held released (except proximity / battery), stops velocity prediction and gives the new sender a full timeout. Reply: `control` (`owner`, per-device `devices`, per-sender `sources`: `age_s`, `accepted`, `ignored`) and `poses` (latest position / rotation of each device, so the sender taking over can continue from there). `status` carries `control` too |
| `hand_mode` | `mode` (`controllers` / `hands`; without it, only returns the current mode) | Switches both hands between controllers and hand tracking (SteamVR keeps running). `{"ok":true,"mode":"hands"}`. `status` also carries `hand_mode` |
| `render_models` | `mode` (`labeled` / `axes`; without it, only returns the current setting) | Switches how controllers and trackers look (SteamVR keeps running). `labeled`: shapes with their name (default); `axes`: colored axes for development (X red, Y green, Z blue, 10 cm; the white line points forward = -Z). The startup default is the config key `renderModels` |
| `preview` | `mode` (`on` / `off`) | Starts / pauses the preview (eye images in shared memory) in direct mode (Linux, or Windows with `display.mode = direct`). It is off by default, so nothing is copied or written while nobody watches (`preview_viewer.py` and `linux_check.py` start and stop it themselves) |
| `refresh_rate` | `hz` (1-240; without it, only returns the current rate) | Changes the virtual display's frame rate (direct mode; SteamVR keeps running). `{"ok":true,"hz":30}` |
| `preview_window` | `mode` (`on` / `off`) | Shows / hides the preview window (the HMD image) in Windows direct mode (`display.mode = direct`). Nothing is done while it is hidden |
| (`perf` in `status`) | | The driver's own load: `update_thread_cpu_pct`, `receive_thread_cpu_pct` (% of one core, refreshed every second), `update_tick_us_avg` / `update_tick_us_max` (time per update) |

`display` in `status`: `mode` (`window` / `direct`), `width`, `height`, `refresh_hz`. In direct mode (Linux), additionally `swap_sets_created`, `swap_sets_alive`, `layers_submitted`, `presents`, `vsyncs`, `eye_texture` (`width`, `height`, `vk_format`), `preview` (`state`, `submitted`, `written`, `active`).

Each device in `status`: `id, serial, kind, role, enabled, on_demand, timeout_policy, index` (SteamVR device index, -1 if not registered)`, pose_packets, input_packets, rate_hz` (receive rate)`, age_s` (seconds since last receipt)`, latency_ms` (if `ts` is present).

### Driver → client: `haptic`

When an app requests haptics, the driver sends it to **the address that last sent a packet**. With VHMD Link, `headset_bridge` forwards it to the headset and the controller vibrates.

```json
{ "type": "haptic", "id": "right_hand", "duration": 0.005, "frequency": 100.0, "amplitude": 0.75 }
```

## Timeouts

A device whose last `pose` is older than `timeout.ms` (500 ms by default) follows its per-device `timeoutPolicy`; if none is set, **on-demand trackers use `disconnect`**, and everything else (HMD, controllers) follows `timeout.policy` (`hold` by default). The not-yet-received state right after startup is handled the same way (measured from startup time).

| policy | Behavior |
|---|---|
| `hold` (default) | Keeps the last pose valid. Velocity is set to 0 so prediction does not make it drift |
| `invalid` | `poseIsValid=false`, `TrackingResult_Running_OutOfRange` |
| `disconnect` | The above + `deviceIsConnected=false` |

## Invalid packets

The following are discarded, and a warning is written to `vrserver.txt` (rate-limited to 5 per second by default, with a count of suppressed messages). The driver does not stop.

- Invalid JSON / wrong type / missing required field / unknown `type`
- NaN, Inf, out-of-range values (position ±1000 m, velocity ±100, input ±1), zero-norm quaternions
- Unknown or disabled device id, skeleton whose bone count is not 31
- Batch of more than 256 entries, OSC with `vmt.enabled=false`, unsupported OSC address

Valid messages in a batch are applied even if other elements are invalid.

## VMT-compatible OSC (optional)

A compatible receiver that lets you use sender tools for [Virtual Motion Tracker](https://gpsnmeajp.github.io/VirtualMotionTrackerDocument/) (0.15) as they are. Enable with `vmt.enabled: true`.

| Address | Support |
|---|---|
| `/VMT/Room/Unity`, `/VMT/Raw/Unity` | ○ Unity coordinates (left-handed) → converted by flipping Z |
| `/VMT/Room/UEuler`, `/VMT/Raw/UEuler` | ○ Unity Euler angles (degrees) |
| `/VMT/Room/Driver`, `/VMT/Raw/Driver` | ○ Driver coordinates as is |
| `/VMT/Joint/{Unity,UEuler,Driver}` | ○ A serial is appended at the end. Pose relative to the position and rotation of the parent device (serial; `HMD` = the HMD). Follows the parent every cycle as it moves |
| `/VMT/Follow/{Unity,UEuler,Driver}` | ○ Same as above, but only the position follows the parent; the rotation stays fixed to the room orientation |
| `/VMT/Input/Button[/Touch]` | ○ buttonIndex 0=system,1=a (left hand x),2=b (left hand y),3=menu,4=joystick,5=trigger,6=grip (click / touch) |
| `/VMT/Input/Trigger[/Touch|/Click]` | ○ triggerIndex 0=trigger, 1=grip, 2=grip_force |
| `/VMT/Input/Joystick[/Touch|/Click]` | ○ joystickIndex 0=joystick, 1=trackpad |
| `/VMT/Skeleton/{Unity,UEuler,Driver}` + `/VMT/Skeleton/Apply` | ○ Bones (0 to 30, in parent bone space) are accumulated and sent on Apply. Bones not sent are identity rotations |
| `/VMT/Skeleton/Scalar` | ○ Curl for boneSet 1 to 5 (thumb to little finger). In VMT, 0 = closed / 1 = open. Only mode 0 |
| `/VMT/SetRoomMatrix[/Temporary]`, `/VMT/Set/RoomMatrix[/Temporary]` | ○ 3×4 room space → driver space matrix (12 values, row-major). Applied to Room poses only. Both last until the driver shuts down (not saved to a file) |
| `/VMT/Reset` | ○ Invalidates all devices driven via VMT (they stay registered) |
| `/VMT/Subscribe/Device`, `/VMT/Unsubscribe/Device` | ○ Sends the pose of the given serial via `/VMT/Out/SubscribedDevice` (driver space) |
| `/VMT/Get/Devices/List` | ○ Returns `/VMT/Out/Devices/List` (a list of `index:serial`) |
| `/VMT/Set/Destination` | ○ Changes the output destination (IP, port) |
| `/VMT/LoadSetting` | ○ Only replies via `/VMT/Out/Log` |
| `/VMT/SetAutoPoseUpdate`, `/VMT/SetDiagLog`, `/VMT/RequestRestart`, `/VMT/Config`, `/VMT/Debug` | Accepted but ignored |

Outputs (default destination 127.0.0.1:39571, same as VMT): `/VMT/Out/Alive` (every second; the version is `VHMD_<version>`), `/VMT/Out/Unavailable` (every second; 0 = OK), `/VMT/Out/SubscribedDevice`, `/VMT/Out/Devices/List`, `/VMT/Out/Log`, `/VMT/Out/Haptic` (when an app requests haptics on a device driven via VMT). Output starts only after at least one VMT message has been received.

- `index` → device: `vmt.indexMap` (e.g. `{"0": "tracker_0", "1": "left_hand"}`); if not specified, `tracker_<index>`.
- `enable` 2/5 → `left_hand`, 3/6 → `right_hand`, 0 → invalidates that device's pose. 4 (tracking reference) and 7 (VIVE Tracker compatible) are treated as trackers.
- Input and skeleton messages go to the device that the index last sent a pose to.
- Unlike VMT, Room poses are accepted even without setting a room matrix (identity matrix).
- VMT-specific button numbers (Button0 to 17, etc.) not listed in the table above are not accepted.
- Since it uses port 39570, the same as VMT itself, do not enable it at the same time as the VMT driver.

## Client implementation example

Python: `clients/python/vhmd_client` (no dependencies).

```python
from vhmd_client import VHMDClient, math3d
with VHMDClient() as c:
    with c.batch():                      # bundle into 1 datagram
        c.pose("hmd", (0, 1.7, 0), math3d.from_euler(yaw=30))
        c.pose("waist", (0, 1.0, 0))
    c.inputs("right_hand", {"trigger": 1.0, "a": True})
    print(c.status()["devices"][0])
```

In other languages, you can implement a client simply by "sending JSON over UDP to `127.0.0.1:39570`".
