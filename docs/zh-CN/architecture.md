[日本語](../architecture.md) | [English](../en/architecture.md) | **简体中文** | [한국어](../ko/architecture.md) — [返回 README](../../README.zh-CN.md)

# 架构

## 整体结构

```
[外部客户端 (Python / 任意语言 / VMT 发送工具)]
        │ UDP 127.0.0.1:39570  (JSON or VMT-OSC)
        ▼
┌───────────────────────── driver_vhmd.dll (位于 vrserver 进程内) ─────────────────────────┐
│ UdpReceiver 线程 ──► MessageRouter ──► DeviceState (每个设备一份, 受 mutex 保护)           │
│   net/udp_*              state/router       state/device_state                            │
│   (仅此处依赖 OS)         protocol/parser    、最新姿态 / 输入 / 骨骼 / 视线                │
│                          protocol/osc       、超时判定、平滑                               │
│                                                    │                                     │
│ 更新线程 (默认 120Hz, 高精度定时器) ────────────────┘                                     │
│   DeviceProvider::UpdateLoop → VirtualDevice::Update                                      │
│     → TrackedDevicePoseUpdated / UpdateBoolean/Scalar/Skeleton/EyeTrackingComponent       │
│                                                                                           │
│ RunFrame (vrserver 主线程) → PollNextEvent → 通过 UDP 将振动事件发给客户端                 │
└───────────────────────────────────────────────────────────────────────────────────────────┘
        ▼
     SteamVR (vrserver) → 应用 (VRChat 等)
```

## 分层

| 层 | 目录 | 依赖 OpenVR | 单元测试 |
|---|---|---|---|
| 数学 | `driver/src/math/` | 否 | ○ |
| 协议(JSON / OSC) | `driver/src/protocol/` | 否 | ○ |
| 配置、角色 | `driver/src/config/` | 否 | ○ |
| 状态、路由 | `driver/src/state/` | 否 | ○ |
| 网络 | `driver/src/net/` | 否(内部切换 WinSock/BSD) | ○(回环) |
| 工具 | `driver/src/util/` | 否 | 部分 |
| 设备、Provider | `driver/src/*.cpp` | 是 | 集成测试 |
| Linux 直接模式 | `driver/src/linux/` | 是(+ Vulkan，运行时加载) | 捕获部分用自测，整体用 `linux_check.py` |

不依赖 OpenVR 的部分汇总在静态库 `vhmd_core` 中，驱动 DLL 和测试可执行文件都会链接它。

## 线程与生命周期

| 线程 | 职责 | 结束 |
|---|---|---|
| vrserver 主线程 | `Init` / `RunFrame` / `Cleanup`、`Activate` / `Deactivate` | — |
| 接收 | `UdpReceiver::Loop`。poll 每 100 ms 超时一次并检查停止标志 | 在 `Cleanup` 中 `Stop()` → join(最长约 100 ms) |
| 更新 | `DeviceProvider::UpdateLoop`。以固定周期更新所有设备 | 在 `Cleanup` 中设置停止标志 → join(最长 1 个周期) |
| vsync(Linux) | `DirectModeComponent::VsyncLoop`。按显示刷新率通知 `VsyncEvent` | 在 HMD 的 `Deactivate` 中停止 |
| 捕获(Linux) | `FrameCapture::Worker`。只接收最新一帧，缩小后写入共享内存 | 销毁设备时 join |
| 合成器(SteamVR 侧) | 调用 `CreateSwapTextureSet` / `SubmitLayer` / `Present` / `PostPresent`。`PostPresent` 会被阻塞到下一次 vsync | — |

- `Init`：读取配置 → 为所有设备生成 `DeviceState` 和设备对象 → 仅对**非按需**设备(默认是 HMD 和左右控制器这 3 点)调用 `TrackedDeviceAdded` → 启动接收、更新线程。
- 按需添加：接收线程收到发往未注册追踪器的消息时，先把值放入缓冲区，再调用 `RequestAdd(id)`(只是加入队列)。`RunFrame`(vrserver 主线程)通过 `ProcessPendingAdds()` 处理队列并调用 `TrackedDeviceAdded`。缓冲的值会在 `Activate` 之后 100 ms 开始发送，因此添加后立即处于同步状态。
- `Activate(index)`：创建属性、输入组件。通过每个设备的 mutex 与更新线程互斥。
- 在 `Activate` 之后 100 ms 内，更新线程不会发送该设备的姿态(如果在 vrserver 完成设备所有权注册之前发送，会出现 `PoseUpdated ... owned by driver (-1)` 错误。已实测确认)。
- `Deactivate`：使索引失效 → 此后更新线程不再访问该设备。
- `Cleanup`：停止接收 → 停止更新 → 销毁设备 → 解除日志输出。已通过连续 31 次启动/退出确认 SteamVR 退出时不会挂起(`docs/verification.md`)。
- 接收处理函数中的所有异常都会被捕获，接收线程会继续运行。

## 数据流约定

- 接收端采用“用最新值覆盖”。不持有队列，即使高频发送(1000 pps 以上)延迟也不会累积。
- 输入值按设备、按名称合并为最新值，由更新线程反映到 SteamVR。
- 延迟构成：UDP 接收是即时的，反映到 SteamVR 要等到下一个更新周期(120 Hz 时最长 8.3 ms)。实测从发送到应用可见的中位数为 8.2 ms(`docs/verification.md`)。
- 姿态在每个周期都通过 `TrackedDevicePoseUpdated` 发送(因为 SteamVR 会把停止更新的设备视为过时)。

## 扩展点

| 想做的事 | 需要修改的地方 |
|---|---|
| 新的设备类型 | 继承 `VirtualDevice`(在 `OnActivate` 中设置属性/组件，重写 `OnInput`/`OnTick` 等)，并添加到 `DeviceKind` 和 `DeviceProvider::CreateDevices` |
| 新的消息类型 | 在 `protocol/messages.h` 中添加结构体 → `parser.cpp` 的 `ParseOne` → `MessageRouter::HandleDatagram` → `DeviceState` → 设备的钩子 |
| 新的传输方式 (WebSocket、共享内存等) | 以与 `UdpReceiver` 相同的方式接收，然后调用 `MessageRouter::HandleDatagram()` 即可(路由器与传输方式无关) |
| 兼容其他协议 | 与 `protocol/osc.*` 一样，添加一个返回 `ParseResult` 的解析器，并加入 `HandleDatagram` 的判定 |
| 利用画面(编码 / 推流 / 录制) | Linux：`linux/frame_capture.cpp` 的 `Capture()` 是接收左右眼画面的地方，可以在共享内存之外添加其他输出。如果 Windows 也要采用相同方式，则用 D3D11 共享纹理实现 `IVRDriverDirectModeComponent` |
| 添加控制器输入 | `controller_device.cpp` 的 `InputDef` 表、`resources/input/vhmd_controller_profile.json`，必要时还有重映射(`tools/gen_remapping.py`)和旧版绑定 |

## 平台抽象(未来的 Linux 支持)

- `util/platform.h`：`VHMD_EXPORT`(`__declspec(dllexport)` / `visibility("default")`)、`kPathSeparator`。
- `net/udp_socket.cpp`：用 `#if` 切换 WinSock 和 BSD 套接字。头文件与 OS 无关(句柄为 `intptr_t`)。
- `util/precise_sleep.cpp`：Windows 使用高精度 Waitable Timer(不通过 `timeBeginPeriod` 修改系统全局定时器)，其他平台使用 `sleep_until`。
- CMake：`CMAKE_CXX_VISIBILITY_PRESET hidden`，输出路径 `bin/<win64|linux64|osx64>/driver_vhmd.<dll|so>`，`PREFIX ""`。
- 配置文件路径通过 `std::filesystem::u8path` 按 UTF-8 处理。
- Linux：已在 WSL2 上完成构建和单元测试。ELF 中通过 `driver_exports.map` 将 `HmdDriverFactory` 以外的符号设为非公开，并静态链接 libstdc++/libgcc。在 Linux 版 SteamVR 上的运行尚未验证(`docs/linux.md`)。

## 向 SteamVR 注册的名称、ID

| 类型 | 值 |
|---|---|
| 驱动名 | `vhmd`(`driver.vrdrivermanifest`) |
| 控制器类型 | `vhmd_hmd`, `vhmd_controller`, `vhmd_tracker`(自有类型，不使用其他厂商名称) |
| 制造商名 | `VirtualHMD (development)` |
| 追踪系统名 | `vhmd` |
| 渲染模型 | SteamVR 自带的通用模型 `generic_hmd` / `generic_controller` / `generic_tracker` |
| 注册设备类型 | `vhmd/<serial>`(对应追踪器角色的设置键 `/devices/vhmd/<serial>`) |
