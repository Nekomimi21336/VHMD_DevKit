[日本語](../protocol.md) | [English](../en/protocol.md) | **简体中文** | [한국어](../ko/protocol.md) — [返回 README](../../README.zh-CN.md)

# 外部输入协议 (UDP / JSON, VMT 兼容 OSC)

驱动 `vhmd` 将通过 UDP 接收到的消息写入各虚拟设备的“最新值缓冲区”，由更新线程(默认 120 Hz)将其反映到 SteamVR。

| 项目 | 值 |
|---|---|
| 传输层 | UDP (IPv4) |
| 默认地址 | `127.0.0.1:39570` (`vhmd_config.json` 的 `network.bind` / `network.port`) |
| 对外开放 | 仅当 `network.allowRemote: true` 时才可绑定到 `127.0.0.1` 以外的地址(默认不可) |
| 格式 | 1 个数据报 = 1 个 JSON 对象，或 JSON 数组(批量，最多 256 条) |
| 最大大小 | 65507 字节(UDP 上限) |
| 字符编码 | UTF-8 |
| VMT 兼容 OSC | 当 `vmt.enabled: true` 时在同一端口接收(首字节为 `/` 或 `#` 时判定为 OSC) |

## 坐标系与单位

- **遵循 SteamVR**：右手系、**+Y 向上**、**-Z 向前**、+X 向右，单位为米。
- 旋转统一使用**四元数 `[w, x, y, z]`**(即使未归一化，也会在接收时归一化)。
- 速度 `vel` 单位为 m/s，角速度 `angvel` 单位为 rad/s(均为世界坐标系)。
- `pos` 是“头部(HMD)的位置”。如果希望将其视为颈部位置并应用 SteamVR 的颈部模型，请设置 `hmd.headModel: true`。
- 姿态直接写入 `TrackingUniverseRawAndUncalibrated`(房间设置的偏移由 SteamVR 侧应用)。

## 设备 ID

| id | 类型 | SteamVR 类 / 角色 | 默认序列号 |
|---|---|---|---|
| `hmd` | HMD | `TrackedDeviceClass_HMD` | `VHMD-HMD-0` |
| `left_hand` | 控制器 | `Controller` / `TrackedControllerRole_LeftHand` | `VHMD-CTL-L` |
| `right_hand` | 控制器 | `Controller` / `TrackedControllerRole_RightHand` | `VHMD-CTL-R` |
| `tracker_0` | 追踪器 | `GenericTracker` / `TrackerRole_Waist` | `VHMD-TRK-0` |
| `tracker_1` | 同上 | `TrackerRole_LeftFoot` | `VHMD-TRK-1` |
| `tracker_2` | 同上 | `TrackerRole_RightFoot` | `VHMD-TRK-2` |
| `tracker_3` | 同上 | `TrackerRole_LeftKnee` | `VHMD-TRK-3` |
| `tracker_4` | 同上 | `TrackerRole_RightKnee` | `VHMD-TRK-4` |
| `tracker_5` | 同上 | `TrackerRole_LeftElbow` | `VHMD-TRK-5` |
| `tracker_6` | 同上 | `TrackerRole_RightElbow` | `VHMD-TRK-6` |
| `tracker_7` | 同上 | `TrackerRole_Chest` | `VHMD-TRK-7` |

- **基础为 3 点**(`hmd`, `left_hand`, `right_hand`)。只有这 3 个设备会在 SteamVR 启动时注册。
- **追踪器按需添加**：发往该追踪器的消息(`pose` 等)首次到达时才添加到 SteamVR，并从收到的姿态开始同步(实测约 40 ms)。发送双脚即为 5 点，再加上腰部即为 6 点。也可以通过 `config` 的 `op: "add"` 预先添加。添加的设备会一直保留到 SteamVR 退出(因为 OpenVR 没有删除设备的 API)，停止发送后变为 `disconnect`(灰色显示)。如果希望启动时就注册全部设备，请设置 `"trackersOnDemand": false`。
- 追踪器通过配置最多 16 个。`id` 和角色可以在 `vhmd_config.json` 的 `trackers` 数组中修改。
- **也可以用角色名指定**：如果同一角色的追踪器只有 1 个，可以像 `"id": "waist"` 这样用简短的角色名发送(等同于 `tracker_0`)。
- 角色名接受 3 种形式：`waist` / `vive_tracker_waist` / `TrackerRole_Waist`。
  列表：`handed, left_foot, right_foot, left_shoulder, right_shoulder, left_elbow, right_elbow, left_knee, right_knee, left_wrist, right_wrist, left_ankle, right_ankle, waist, chest, camera, keyboard`(SteamVR 2.17.10 自带配置文件中存在的角色)。

## 消息

### `pose`

```json
{ "type": "pose", "id": "hmd",
  "pos": [0.0, 1.7, 0.0], "rot": [1.0, 0.0, 0.0, 0.0],
  "vel": [0, 0, 0], "angvel": [0, 0, 0], "ts": 1760000000.123 }
```

| 字段 | 必需 | 说明 |
|---|---|---|
| `pos` | ○ | [x, y, z] m。各分量的绝对值 ≤ 1000 |
| `rot` | ○ | [w, x, y, z]。范数为 0 / NaN 视为非法 |
| `vel`, `angvel` | | [x, y, z]。绝对值 ≤ 100。用于 SteamVR 的预测(缺省为 0) |
| `acc`, `angacc` | | 加速度(m/s²、rad/s²)[x, y, z]。绝对值 ≤ 1000。直接传给 SteamVR |
| `age` | | 样本在发送时已经过时多少秒(0〜0.5)。SteamVR 从采样时刻开始预测(`poseTimeOffset`)。适用于摄像头估计等存在延迟的发送端 |
| `ts` | | 发送端 UNIX 时间(秒)。用于诊断(`status` 的 `latency_ms`) |
| `valid` | | 设为 `false` 可显式表示“追踪丢失”。可省略 `pos`/`rot`。收到下一个有效的 `pose` 时恢复 |

### `input`(控制器 / 追踪器 / HMD)

```json
{ "type": "input", "id": "right_hand", "name": "trigger", "value": 0.8 }
{ "type": "input", "id": "right_hand", "values": { "trigger": 0.8, "a": true, "joystick_x": -0.3 } }
```

值为数值(-1〜1)或布尔值。按键在 0.5 及以上时视为按下。

| 名称 | SteamVR 输入路径 | 类型 / 范围 |
|---|---|---|
| `trigger` | `/input/trigger/value` | 0〜1 |
| `trigger_click` / `trigger_touch` | `/input/trigger/click` / `touch` | bool |
| `grip` | `/input/grip/value` | 0〜1 |
| `grip_click` / `grip_touch` | `/input/grip/click` / `touch` | bool |
| `joystick_x`, `joystick_y` | `/input/joystick/x`, `y` | -1〜1(向上为 +y) |
| `joystick_click` / `joystick_touch` | `/input/joystick/click` / `touch` | bool |
| `a`, `b` (右手) / `x`, `y` (左手) | `/input/a/click` 等 | bool |
| `a_touch`, `b_touch`, `x_touch`, `y_touch` | `/input/a/touch` 等 | bool |
| `system`, `system_touch` | `/input/system/click` / `touch` | bool |
| `menu` | `/input/menu/click` | bool |
| `trackpad_x`, `trackpad_y` | `/input/trackpad/x`, `y` | -1〜1 |
| `trackpad_click` / `trackpad_touch` | `/input/trackpad/click` / `touch` | bool |
| `trackpad_force` | `/input/trackpad/force` | 0〜1 |
| `grip_force` | `/input/grip/force` | 0〜1 |
| `thumbrest_touch` | `/input/thumbrest/touch` | bool |

追踪器(相当于实体追踪器的 POGO 针脚输入)：

| 名称 | SteamVR 输入路径 | 类型 / 范围 |
|---|---|---|
| `system`, `menu`, `grip` | `/input/system/click`, `/input/menu/click`, `/input/grip/click` | bool |
| `trigger` / `trigger_click` | `/input/trigger/value` / `click` | 0〜1 / bool |
| `trackpad_x`, `trackpad_y`, `trackpad_click`, `trackpad_touch` | `/input/trackpad/...` | -1〜1 / bool |

HMD：

| 名称 | SteamVR 输入路径 | 类型 / 范围 |
|---|---|---|
| `system` | `/input/system/click` | bool |
| `proximity` | `/proximity` | bool。佩戴传感器(true = 佩戴中，默认值为配置项 `hmd.proximity`)。应用会以 `k_EButton_ProximitySensor`、`/user/head/proximity` 以及 OpenXR 的 `XR_EXT_user_presence` 的形式收到 |

所有设备通用：

| 名称 | 内容 |
|---|---|
| `battery` | 电池电量 0〜1(`Prop_DeviceBatteryPercentage_Float`)。控制器和追踪器在发送之前为 1.0(满电)，HMD 在发送后变为带电池的设备 |
| `charging` | 充电中(`Prop_DeviceIsCharging_Bool`) |

- 别名：`primary` = 右手 `a` / 左手 `x`，`secondary` = 右手 `b` / 左手 `y`。向左手发送 `a`/`b` 时按 `x`/`y` 处理，向右手发送 `x`/`y` 时按 `a`/`b` 处理。`thumbstick_*`, `stick_*` = `joystick_*`，`squeeze` = `grip`，`application_menu` = `menu`，`touchpad_*` = `trackpad_*`。
- 也可以直接使用原始路径(例如 `"/input/y/click"`)。
- 只发送 `trigger` / `grip` 时，会自动生成 `*_click`(0.75 时 ON / 0.65 时 OFF)和 `*_touch`(0.05 / 0.02)。只要显式发送过一次 `*_click` / `*_touch`，该项的自动生成即停止。
- 触控板和握把在输入配置文件中带有 force，因此 SteamVR 会根据 force 的值生成点击。在客户端发送 `*_force` 之前，`*_click` 也会同步反映到 force 上(只发送点击也能按下)。
- 在同一更新周期内到达的值会按名称合并为最新值。

### `skeleton`(左右控制器的手指)

```json
{ "type": "skeleton", "id": "left_hand", "curl": [0.0, 1.0, 1.0, 1.0, 1.0], "splay": [0, 0, 0, 0, 0] }
{ "type": "skeleton", "id": "left_hand", "bones": [[w,x,y,z], ... 31 个], "positions": [[x,y,z], ... 31 个] }
```

- `curl`：拇指、食指、中指、无名指、小指的弯曲程度，0(张开)〜1(握紧)。`splay`：-1〜1(省略时为 0)。使用 OpenVR 示例 `handskeletonsimulation` 的手部模型生成骨骼。
- `bones`：OpenVR 手部骨骼(31 根骨骼，按 `HandSkeletonBone` 顺序)在**父骨骼空间**中的旋转。可以用 `[w,x,y,z,px,py,pz]` 同时指定位置，也可以单独传入 `positions`(省略时使用张开手的参考位置)。
- 自最后一条 `skeleton` 起 500 ms 以上没有收到任何内容期间，会根据 `trigger`/`grip`/按键的触摸状态自动生成手指弯曲。
- 向 SteamVR 的 `WithController` / `WithoutController` 两种运动范围发送相同的值。
- 控制器与手部追踪可以**在不停止 SteamVR 的情况下切换**(`config` 的 `hand_mode`)。`controllers`(默认)与硬件控制器的处理方式相同，骨骼为 Partial(由 SteamVR 整理手指骨骼)。`hands` 时每只手换成另一个设备(`VHMD-HAND-L/R`，骨骼为 Full，发送的骨骼原样反映)，VRChat 等会将其视为手部追踪(此时按键菜单操作无效，改为手势操作)。两种模式下 ID 都保持 `left_hand` / `right_hand`，发送内容无需修改。手部设备在首次切换到 `hands` 时添加。两种模式下都可以用激光 + 扳机操作 VRChat 菜单。自动操作瞄准时，请以接近正面的角度照射菜单表面(角度过浅不会命中)。

### `gaze`(HMD 的视线)

```json
{ "type": "gaze", "id": "hmd", "origin": [0, 0, 0], "dir": [0, 0, -1] }
{ "type": "gaze", "id": "hmd", "origin": [0, 0, 0], "target": [0.1, 0.0, -1.0] }
{ "type": "gaze", "id": "hmd", "valid": false }
```

以 `VREyeTrackingData_t`(origin, target)的形式传给 `IVRDriverInput::CreateEyeTrackingComponent("/eyetracking")`。

- **坐标为 HMD 局部空间**(米、右手系、−Z 为前方)。`origin` 是眼睛位置，`target` 是注视点(是点而不是方向；发送 `dir` 时注视点为 `origin + dir`)。SteamVR 会乘上 HMD 的姿态后再交给应用(已实测确认)。
- 应用侧读取：OpenVR 使用 `IVRInput::GetEyeTrackingDataRelativeToNow` / `GetEyeTrackingDataForNextFrame`(将 `eyetracking` 类型的动作绑定到 `/user/head/eyetracking`)。OpenXR 使用 `XR_EXT_eye_gaze_interaction`(`/user/eyes_ext/input/gaze_ext/pose`)。两者均已在集成测试中确认。
- 发送 `valid: false` 时，OpenVR 的 `bValid` / `bTracked` 为 false，OpenXR 的位置有效标志变为 0。
- 只有双眼合并的一条视线(OpenVR 的规格，没有左右眼分别的数据或眼睑开合度)。

### `config`(请求、响应)

```json
{ "type": "config", "op": "list", "seq": 1 }
```

`seq` 会原样返回到响应中。响应通过 UDP 发回发送方的地址/端口。

| op | 参数 | 响应 |
|---|---|---|
| `list` (`status`) | | `{"type":"status", "devices":[...], "packets":{"total","rejected"}, ...}` |
| `ping` | | `{"type":"result","op":"ping","ok":true,"version":"0.1.0"}` |
| `roles` | | 可用角色列表 |
| `set_role` | `id`, `role` | 将角色写入 SteamVR 设置的 `trackers` 部分(覆盖用户设置) |
| `reset` | `id`(省略时为全部设备) | 恢复为配置文件中的默认姿态 |
| `add` | `id`(省略时为全部按需设备) | 立即将按需设备添加到 SteamVR(下一帧生效)。`{"ok":true,"ids":[...]}` |
| `hand_mode` | `mode`(`controllers` / `hands`，省略时仅返回当前模式) | 在控制器 / 手部追踪之间切换左右手(SteamVR 不会停止)。`{"ok":true,"mode":"hands"}`。`status` 中也包含 `hand_mode` |
| `preview` | `mode`(`on` / `off`) | 在直接模式下(Linux，或 Windows 上 `display.mode = direct`)开始 / 暂停预览(向共享内存写出画面)。默认关闭，无人查看时不进行 GPU 复制和写入(`preview_viewer.py` 和 `linux_check.py` 会自动开始和停止) |
| `refresh_rate` | `hz`(1〜240，省略时仅返回当前值) | 更改虚拟显示器的帧率(直接模式，SteamVR 不会停止)。`{"ok":true,"hz":30}` |
| `preview_window` | `mode`(`on` / `off`) | 在 Windows 直接模式(`display.mode = direct`)下显示 / 隐藏预览窗口(HMD 画面)。隐藏期间不进行任何处理 |
| (`status` 中的 `perf`) | | 驱动自身的负载：`update_thread_cpu_pct`、`receive_thread_cpu_pct`(占单核的 %，每秒更新)、`update_tick_us_avg` / `update_tick_us_max`(每次更新的处理时间) |

`status` 的 `display`：`mode`(`window` / `direct`)、`width`、`height`、`refresh_hz`。在直接模式(Linux)下还包含 `swap_sets_created`、`swap_sets_alive`、`layers_submitted`、`presents`、`vsyncs`、`eye_texture`(`width`、`height`、`vk_format`)、`preview`(`state`、`submitted`、`written`、`active`)。

`status` 中每个设备包含：`id, serial, kind, role, enabled, on_demand, timeout_policy, index`(SteamVR 设备编号，未注册为 -1)`, pose_packets, input_packets, rate_hz`(接收速率)`, age_s`(距最后接收的秒数)`, latency_ms`(有 `ts` 时)。

### 驱动 → 客户端：`haptic`

当应用请求振动时，会发送到**最后一个发来数据包的地址**。

```json
{ "type": "haptic", "id": "right_hand", "duration": 0.005, "frequency": 100.0, "amplitude": 0.75 }
```

## 超时

距最后一个 `pose` 超过 `timeout.ms`(默认 500 ms)的设备，按各设备的 `timeoutPolicy` 处理；若未设置，则**按需追踪器使用 `disconnect`**，其他设备(HMD、控制器)使用 `timeout.policy`(默认 `hold`)。启动后尚未收到数据的状态也同样处理(从启动时刻开始计时)。

| policy | 行为 |
|---|---|
| `hold`(默认) | 保持最后的姿态为有效。速度置为 0，以免因预测而漂移 |
| `invalid` | `poseIsValid=false`, `TrackingResult_Running_OutOfRange` |
| `disconnect` | 上述 + `deviceIsConnected=false` |

## 非法数据包

以下数据包会被丢弃，并在 `vrserver.txt` 中输出警告(默认限速为每秒 5 条，附带被抑制的条数)。驱动不会停止。

- JSON 非法 / 类型错误 / 缺少必需字段 / 未知的 `type`
- NaN、Inf、超出范围的值(位置 ±1000 m、速度 ±100、输入 ±1)、范数为 0 的四元数
- 未知或已禁用的设备 id、骨骼数不为 31 的 skeleton
- 批量超过 256 条、`vmt.enabled=false` 时的 OSC、不支持的 OSC 地址

即使批量中有其他元素非法，其中的正常消息仍会被应用。

## VMT 兼容 OSC(可选)

这是为了直接使用 [Virtual Motion Tracker](https://gpsnmeajp.github.io/VirtualMotionTrackerDocument/)(0.15)的发送端工具而实现的兼容接收端。通过 `vmt.enabled: true` 启用。

| 地址 | 支持情况 |
|---|---|
| `/VMT/Room/Unity`, `/VMT/Raw/Unity` | ○ Unity 坐标(左手系)→ 反转 Z 后转换 |
| `/VMT/Room/UEuler`, `/VMT/Raw/UEuler` | ○ Unity 欧拉角(度) |
| `/VMT/Room/Driver`, `/VMT/Raw/Driver` | ○ 直接使用驱动坐标 |
| `/VMT/Joint/{Unity,UEuler,Driver}` | ○ 末尾附加 serial。相对于父设备(serial，`HMD` = HMD)位置和旋转的相对姿态。父设备移动时每个周期都会跟随 |
| `/VMT/Follow/{Unity,UEuler,Driver}` | ○ 同上。仅位置跟随父设备，旋转固定为房间朝向 |
| `/VMT/Input/Button[/Touch]` | ○ buttonIndex 0=system,1=a(左手 x),2=b(左手 y),3=menu,4=joystick,5=trigger,6=grip(点击 / 触摸) |
| `/VMT/Input/Trigger[/Touch|/Click]` | ○ triggerIndex 0=trigger, 1=grip, 2=grip_force |
| `/VMT/Input/Joystick[/Touch|/Click]` | ○ joystickIndex 0=joystick, 1=trackpad |
| `/VMT/Skeleton/{Unity,UEuler,Driver}` + `/VMT/Skeleton/Apply` | ○ 先累积骨骼(0〜30，父骨骼空间)，在 Apply 时发送。未发送的骨骼为单位旋转 |
| `/VMT/Skeleton/Scalar` | ○ boneSet 1〜5(拇指〜小指)的弯曲。VMT 中 0 = 握紧 / 1 = 张开。mode 仅支持 0 |
| `/VMT/SetRoomMatrix[/Temporary]`, `/VMT/Set/RoomMatrix[/Temporary]` | ○ 房间空间 → 驱动空间的 3×4 矩阵(行优先，12 个值)。仅应用于 Room 系列的姿态。两者均保持到驱动退出(不保存到文件) |
| `/VMT/Reset` | ○ 使所有由 VMT 驱动的设备无效(注册保留) |
| `/VMT/Subscribe/Device`, `/VMT/Unsubscribe/Device` | ○ 通过 `/VMT/Out/SubscribedDevice` 发送指定 serial 的姿态(驱动空间) |
| `/VMT/Get/Devices/List` | ○ 返回 `/VMT/Out/Devices/List`(`index:serial` 列表) |
| `/VMT/Set/Destination` | ○ 更改输出目标(IP、端口) |
| `/VMT/LoadSetting` | ○ 仅通过 `/VMT/Out/Log` 应答 |
| `/VMT/SetAutoPoseUpdate`, `/VMT/SetDiagLog`, `/VMT/RequestRestart`, `/VMT/Config`, `/VMT/Debug` | 接受但不做任何处理 |

输出(默认目标 127.0.0.1:39571，与 VMT 相同)：`/VMT/Out/Alive`(每秒一次，版本为 `VHMD_<版本>`)、`/VMT/Out/Unavailable`(每秒一次，0 = 正常)、`/VMT/Out/SubscribedDevice`、`/VMT/Out/Devices/List`、`/VMT/Out/Log`、`/VMT/Out/Haptic`(应用向由 VMT 驱动的设备请求振动时)。收到至少一条 VMT 消息后才开始输出。

- `index` → 设备：`vmt.indexMap`(例 `{"0": "tracker_0", "1": "left_hand"}`)，未指定时为 `tracker_<index>`。
- `enable` 2/5 → `left_hand`，3/6 → `right_hand`，0 → 使该设备的姿态无效。4(追踪参考点)和 7(VIVE 追踪器兼容)按追踪器处理。
- 输入和骨骼消息会送达该 index 最后发送姿态的设备。
- 与 VMT 不同，即使未设置房间矩阵，也会接受 Room 系列的姿态(使用单位矩阵)。
- VMT 特有的按键编号(Button0〜17 等)中，不在上表中的不予接受。
- 由于与 VMT 本体使用相同的端口 39570，请勿与 VMT 驱动同时启用。

## 客户端实现示例

Python：`clients/python/vhmd_client`(无依赖)。

```python
from vhmd_client import VHMDClient, math3d
with VHMDClient() as c:
    with c.batch():                      # 合并为 1 个数据报
        c.pose("hmd", (0, 1.7, 0), math3d.from_euler(yaw=30))
        c.pose("waist", (0, 1.0, 0))
    c.inputs("right_hand", {"trigger": 1.0, "a": True})
    print(c.status()["devices"][0])
```

其他语言只需“将 JSON 通过 UDP 发送到 `127.0.0.1:39570`”即可实现。
