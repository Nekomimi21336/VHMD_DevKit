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
| `ts` | | Sender's UNIX time (seconds). For diagnostics (`latency_ms` in `status`) |
| `valid` | | `false` explicitly signals "tracking lost". `pos`/`rot` may be omitted. Recovers on the next valid `pose` |

### `input` (controllers / HMD)

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

- Aliases: `primary` = right-hand `a` / left-hand `x`, `secondary` = right-hand `b` / left-hand `y`. Sending `a`/`b` to the left hand is treated as `x`/`y`, and sending `x`/`y` to the right hand as `a`/`b`. `thumbstick_*`, `stick_*` = `joystick_*`, `squeeze` = `grip`, `application_menu` = `menu`.
- Raw paths (e.g. `"/input/y/click"`) can also be used as is.
- If only `trigger` / `grip` is sent, `*_click` (ON at 0.75 / OFF at 0.65) and `*_touch` (0.05 / 0.02) are generated automatically. Once `*_click` / `*_touch` is sent explicitly even once, auto-generation stops for that item.
- The HMD supports `system` only.
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

### `gaze` (HMD eye gaze)

```json
{ "type": "gaze", "id": "hmd", "origin": [0, 0, 0], "dir": [0, 0, -1] }
{ "type": "gaze", "id": "hmd", "origin": [0, 0, 0], "target": [0.1, 0.0, -1.0] }
{ "type": "gaze", "id": "hmd", "valid": false }
```

Passed to `IVRDriverInput::CreateEyeTrackingComponent("/eyetracking")` as `VREyeTrackingData_t` (origin, target). The coordinate space is not documented in the SteamVR headers; **HMD-local space is assumed (unverified)**. See `docs/decisions.md` (Japanese).

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

`display` in `status`: `mode` (`window` / `direct`), `width`, `height`, `refresh_hz`. In direct mode (Linux), additionally `swap_sets_created`, `swap_sets_alive`, `layers_submitted`, `presents`, `vsyncs`, `eye_texture` (`width`, `height`, `vk_format`), `preview` (`state`, `submitted`, `written`, `active`).

Each device in `status`: `id, serial, kind, role, enabled, on_demand, timeout_policy, index` (SteamVR device index, -1 if not registered)`, pose_packets, input_packets, rate_hz` (receive rate)`, age_s` (seconds since last receipt)`, latency_ms` (if `ts` is present).

### Driver → client: `haptic`

When an app requests haptics, the driver sends it to **the address that last sent a packet**.

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

A subset that lets you use sender tools for [Virtual Motion Tracker](https://gpsnmeajp.github.io/VirtualMotionTrackerDocument/) as they are. Enable with `vmt.enabled: true`.

| Address | Support |
|---|---|
| `/VMT/Room/Unity`, `/VMT/Raw/Unity` | ○ Unity coordinates (left-handed) → converted by flipping Z |
| `/VMT/Room/UEuler`, `/VMT/Raw/UEuler` | ○ Unity Euler angles (degrees) |
| `/VMT/Room/Driver`, `/VMT/Raw/Driver` | ○ Driver coordinates as is |
| `/VMT/Input/Button[/Touch]` | ○ buttonIndex 0=system,1=a (left hand x),2=b (left hand y),3=menu,4=joystick_click,5=trigger_click,6=grip_click |
| `/VMT/Input/Trigger[/Touch|/Click]` | ○ triggerIndex 0=trigger, 1=grip |
| `/VMT/Input/Joystick[/Touch|/Click]` | ○ joystickIndex 0 only |
| `/VMT/Joint/*`, `/VMT/Follow/*`, `/VMT/Skeleton/*`, `/VMT/SetRoomMatrix` etc. | × Not supported (discarded and logged) |

- `index` → device: `vmt.indexMap` (e.g. `{"0": "tracker_0", "1": "left_hand"}`); if not specified, `tracker_<index>`.
- `enable` 2/5 → `left_hand`, 3/6 → `right_hand`, 0 → invalidates that device's pose.
- Room and Raw are not distinguished (the room matrix is not implemented).
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
