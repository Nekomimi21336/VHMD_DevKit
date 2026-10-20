[日本語](../architecture.md) | [English](../en/architecture.md) | **简体中文** | [한국어](../ko/architecture.md) — [返回 README](../../README.zh-CN.md)

# 架构

## 整体结构

```
[外部客户端 (Python / 任意语言 / VMT 发送工具)]
        │ UDP 127.0.0.1:39570  (JSON or VMT-OSC)
        ▼
┌──────────────────────── driver_vhmd.dll / .so (位于 vrserver 进程内) ────────────────────────┐
│ UdpReceiver 线程 ──► MessageRouter ──► DeviceState (每个设备一份, 受 mutex 保护)              │
│   net/udp_*              state/router       state/device_state                               │
│   (仅此处依赖 OS)         protocol/parser    、最新姿态 / 输入 / 骨骼 / 视线                   │
│                          protocol/osc       、超时判定、平滑                                  │
│                                                    │                                         │
│ 更新线程 (默认 120Hz, 高精度定时器) ────────────────┘                                         │
│   DeviceProvider::UpdateLoop → VirtualDevice::Update(只把有变化的发给 SteamVR)               │
│     → TrackedDevicePoseUpdated / UpdateBoolean/Scalar/Skeleton/EyeTrackingComponent          │
│                                                                                              │
│ RunFrame (vrserver 主线程) → 按需添加、通过 UDP 将振动事件发给客户端                          │
│                                                                                              │
│ HMD 的显示(二选一):                                                                          │
│   窗口模式 … IVRDisplayComponent。合成器在桌面上打开窗口(Windows)                           │
│   直接模式 … IVRDriverDirectModeComponent。驱动接收合成器渲染的左右眼画面，                   │
│              vsync 也由驱动产生(Linux：Vulkan，Windows：D3D11)                               │
│              → 预览窗口(Windows)/ 共享内存(供工具、Python 使用)                             │
└──────────────────────────────────────────────────────────────────────────────────────────────┘
        ▼
     SteamVR (vrserver / vrcompositor) → 应用 (VRChat 等)
```

## 分层

| 层 | 目录 | 依赖 OpenVR | 单元测试 |
|---|---|---|---|
| 数学 | `driver/src/math/` | 否 | ○ |
| 协议(JSON / OSC) | `driver/src/protocol/` | 否 | ○ |
| 配置、角色 | `driver/src/config/` | 否 | ○ |
| 状态、路由 | `driver/src/state/` | 否 | ○ |
| 网络 | `driver/src/net/` | 否(内部切换 WinSock/BSD) | ○(回环) |
| 显示用标签(生成模型、图标) | `driver/src/visuals/` | 否(内置点阵字体和 PNG 输出) | ○ |
| 工具 | `driver/src/util/` | 否 | 部分 |
| 设备、Provider | `driver/src/*.cpp` | 是 | 集成测试 |
| Linux 直接模式 | `driver/src/linux/` | 是(+ Vulkan，运行时加载) | 捕获部分用自测，整体用 `linux_check.py` |
| Windows 直接模式 | `driver/src/windows/` | 是(+ D3D11 / DXGI) | 集成测试(以 `display.mode = direct` 运行) |

不依赖 OpenVR 的部分汇总在静态库 `vhmd_core` 中，驱动 DLL 和测试可执行文件都会链接它。

## 线程与生命周期

| 线程 | 职责 | 结束 |
|---|---|---|
| vrserver 主线程 | `Init` / `RunFrame` / `Cleanup`、`Activate` / `Deactivate` | — |
| 接收 | `UdpReceiver::Loop`。poll 每 100 ms 超时一次并检查停止标志 | 在 `Cleanup` 中 `Stop()` → join(最长约 100 ms) |
| 更新 | `DeviceProvider::UpdateLoop`。以固定周期更新所有设备，并每秒测量一次自身负载(`status` 的 `perf`) | 在 `Cleanup` 中设置停止标志 → join(最长 1 个周期) |
| vsync(直接模式) | `DirectModeComponent::VsyncLoop`。按 `refreshHz` 的周期通知 `VsyncEvent`(运行中可更改) | 在 HMD 的 `Deactivate` 中停止 |
| 捕获(Linux) | `FrameCapture::Worker`。仅在被请求期间把最新一帧缩小后写入共享内存 | 销毁设备时 join |
| 预览窗口(Windows) | `DirectModeComponent::WindowLoop`。仅在显示期间把左右眼画面绘制到窗口 | 在 HMD 的 `Deactivate` 中停止 |
| 回读(Windows) | `DirectModeComponent::ReadbackLoop`。仅在被请求期间从 GPU 回读并写入共享内存 | 在 HMD 的 `Deactivate` 中停止 |
| 合成器(SteamVR 侧) | 调用 `CreateSwapTextureSet` / `SubmitLayer` / `Present` / `PostPresent`。`PostPresent` 会被阻塞到下一次 vsync | — |

- `Init`：读取配置 → 为所有设备生成 `DeviceState` 和设备对象 → 仅对**非按需**设备(默认是 HMD 和左右控制器这 3 点)调用 `TrackedDeviceAdded` → 启动接收、更新线程。
- 按需添加：接收线程收到发往未注册追踪器的消息时，先把值放入缓冲区，再调用 `RequestAdd(id)`(只是加入队列)。`RunFrame`(vrserver 主线程)通过 `ProcessPendingAdds()` 处理队列并调用 `TrackedDeviceAdded`。缓冲的值会在 `Activate` 之后 100 ms 开始发送，因此添加后立即处于同步状态。
- 手部模式：左右手各有“控制器”和“手部”(`VHMD-HAND-L/R`)两个设备，共享同一个 `DeviceState`(手部设备在首次切换为 `hands` 时添加)。更新线程只读取一次状态并传给两者(因为输入和骨骼读取后即被清除)。只有当前选中的一方会被报告为已连接。
- `Activate(index)`：创建属性、输入组件，生成显示用标签(模型和图标)。通过每个设备的 mutex 与更新线程互斥。
- 在 `Activate` 之后 100 ms 内，更新线程不会发送该设备的姿态(如果在 vrserver 完成设备所有权注册之前发送，会出现 `PoseUpdated ... owned by driver (-1)` 错误。已实测确认)。
- `Deactivate`：使索引失效 → 此后更新线程不再访问该设备。
- `Cleanup`：停止接收 → 停止更新 → 销毁设备 → 解除日志输出。已通过连续 31 次启动/退出确认 SteamVR 退出时不会挂起(`docs/verification.md`)。
- 接收处理函数中的所有异常都会被捕获，接收线程会继续运行。

## 数据流约定

- 接收端采用“用最新值覆盖”。不持有队列，即使高频发送(1000 pps 以上)延迟也不会累积。
- 输入值按设备、按名称合并为最新值，由更新线程反映到 SteamVR。
- 延迟构成：UDP 接收是即时的，反映到 SteamVR 要等到下一个更新周期(120 Hz 时最长 8.3 ms)。实测从发送到应用可见的中位数为 8.2 ms(`docs/verification.md`)。
- **不做无用的发送**：没有变化的姿态(速度为 0)只每 250 ms 重发一次。待机中的设备(手部模式下未被选中的一方)每 1 秒一次。由按键生成的手指骨骼只在值变化时计算。
- **只在有人查看时处理画面**：预览(共享内存)只在通过 `preview` 操作请求时、预览窗口只在显示期间进行 GPU 复制和写出。
- 套接字和共享图像的句柄不会被子进程继承(以免 vrserver 异常退出后子进程仍占用端口)。

## 扩展点

| 想做的事 | 需要修改的地方 |
|---|---|
| 新的设备类型 | 继承 `VirtualDevice`(在 `OnActivate` 中设置属性/组件，重写 `OnInput`/`OnTick` 等)，并添加到 `DeviceKind` 和 `DeviceProvider::CreateDevices` |
| 新的消息类型 | 在 `protocol/messages.h` 中添加结构体 → `parser.cpp` 的 `ParseOne` → `MessageRouter::HandleDatagram` → `DeviceState` → 设备的钩子 |
| 新的 `config` 操作 | 在 `router.cpp` 的 `HandleConfig` 中添加分支，驱动侧的处理作为 `RouterHooks` 的函数在 `DeviceProvider::Init` 中传入 |
| 新的传输方式 (WebSocket、共享内存等) | 以与 `UdpReceiver` 相同的方式接收，然后调用 `MessageRouter::HandleDatagram()` 即可(路由器与传输方式无关) |
| 兼容其他协议 | 与 `protocol/osc.*` 一样，添加一个返回 `ParseResult` 的解析器，并加入 `HandleDatagram` 的判定 |
| 利用画面(编码 / 推流 / 录制) | 接收左右眼画面的地方：Linux 为 `linux/frame_capture.cpp` 的 `Capture()`，Windows 为 `windows/direct_mode.cpp` 的 `CopyPreview()`。可以在共享内存之外添加其他输出 |
| 添加控制器输入 | `controller_device.cpp` 的 `InputDef` 表、`resources/input/vhmd_controller_profile.json`，必要时还有重映射(`tools/gen_remapping.py`)和旧版绑定 |
| 显示用标签的外观 | `visuals/device_visuals.cpp`(点阵字体、网格、图标颜色) |

## 平台抽象

- `util/platform.h`：`VHMD_EXPORT`(`__declspec(dllexport)` / `visibility("default")`)、`kPathSeparator`。
- `net/udp_socket.cpp`：用 `#if` 切换 WinSock 和 BSD 套接字。头文件与 OS 无关(句柄为 `intptr_t`)。
- `util/precise_sleep.cpp`：Windows 使用高精度 Waitable Timer(不通过 `timeBeginPeriod` 修改系统全局定时器)，其他平台使用 `sleep_until`。
- `util/thread_cpu.cpp`：线程的 CPU 时间(`GetThreadTimes` / `CLOCK_THREAD_CPUTIME_ID`)。
- 直接模式在 `linux/` 和 `windows/` 中以相同的类名、相同的公开函数分别实现，由 `hmd_device.cpp` 根据平台引入。
- CMake：`CMAKE_CXX_VISIBILITY_PRESET hidden`，输出路径 `bin/<win64|linux64|osx64>/driver_vhmd.<dll|so>`，`PREFIX ""`。
- 配置文件路径通过 `std::filesystem::u8path` 按 UTF-8 处理。
- Linux：ELF 中通过 `driver_exports.map` 将 `HmdDriverFactory` 以外的符号设为非公开，并静态链接 libstdc++/libgcc。已在使用真实 GPU 的 Linux 版 SteamVR 上确认(`docs/linux.md`)。

## 向 SteamVR 注册的名称、ID

| 类型 | 值 |
|---|---|
| 驱动名 | `vhmd`(`driver.vrdrivermanifest`) |
| 控制器类型 | `vhmd_hmd`, `vhmd_controller`, `vhmd_tracker`(自有类型，不使用其他厂商名称) |
| 制造商名 | `VirtualHMD (development)` |
| 追踪系统名 | `vhmd` |
| 追踪宇宙(universe) | `0x56484D44`(所有设备共用)。HMD 提供驱动自带的 chaperone(坐姿与站姿原点均在原点，2 × 2 m，边界不显示) |
| 序列号 | `VHMD-HMD-0`、`VHMD-CTL-L/R`、`VHMD-HAND-L/R`(手部模式)、`VHMD-TRK-0`〜 |
| 渲染模型 | HMD 使用 SteamVR 自带的 `generic_hmd`。控制器和追踪器使用驱动在启动时生成的带标签模型(`resources/rendermodels/vhmd_<serial>_<label>/`，含指尖等部件)。无法生成时使用 `{vhmd}vhmd_controller` / `generic_tracker` |
| 状态图标 | 控制器、追踪器使用带简短标记的图标(`resources/icons/labels/`，启动时生成) |
| 注册设备类型 | `vhmd/<serial>`(对应追踪器角色的设置键 `/devices/vhmd/<serial>`) |
