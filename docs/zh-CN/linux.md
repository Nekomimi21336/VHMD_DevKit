[日本語](../linux.md) | [English](../en/linux.md) | **简体中文** | [한국어](../ko/linux.md) — [返回 README](../../README.zh-CN.md)

# 在 Linux 版 SteamVR 上使用

本文介绍让 Linux 原生 SteamVR 加载 `driver_vhmd.so` 的步骤。通过 Proton 运行的 Windows 游戏(VRChat 等)同样连接到 Linux 版 SteamVR，因此需要本驱动。发送端的协议和 Python 客户端与 Windows 通用。

## 原理：在 Linux 上以“直接模式”运行

Linux 版 SteamVR 的合成器无法像 Windows 那样“绘制到桌面上的窗口”。如果没有可通过直接模式(DRM 租用)获得的显示目标，它会报 `VRInitError_Compositor_CannotDRMLeaseDisplay` 并停止启动。SteamVR 自带的 null 驱动也是如此，Valve 也将其作为已知问题处理([SteamVR-for-Linux#889](https://github.com/ValveSoftware/SteamVR-for-Linux/issues/889))。

因此从 v0.3.0 起，Linux 上的虚拟 HMD 实现了 **`IVRDriverDirectModeComponent`**。

```
应用 ──▶ vrcompositor ──(共享 Vulkan 图像 / dma-buf)──▶ driver_vhmd.so
                                                         ├─ 自行通知 vsync(默认 90 Hz)
                                                         └─ 将最新帧写入 /dev/shm/vhmd_preview(可选)
```

- 既不需要真实显示器，也不需要 DRM 租用。左右眼图像绘制在驱动分配的共享 Vulkan 图像上，并通过 `Present` 传递(`IVRIPCResourceManagerClient` 的 Linux 专用 API)。
- 与 ALVR 的 Linux 驱动(2026-10 的 master)方式相同。没有复用其代码，而是依照 OpenVR 头文件自行实现。
- Windows 仍沿用以往的桌面窗口方式(`display.mode`：`auto` / `window` / `direct`)。

## 状态(截至 2026-10-18)

| 项目 | 状态 |
|---|---|
| Linux 构建(gcc 13.3、Ubuntu 24.04 / WSL2) | ✅ 0 个警告(`-Wall -Wextra -Wpedantic`) |
| C++ 单元测试 37 项 | ✅ 全部通过(ASan / TSan 下也无问题) |
| 帧捕获自测(`tests/linux/capture_selftest.cpp`) | ✅ 用真实 Vulkan(Mesa llvmpipe)确认了 fd 导入 → 缩小 → 共享内存 → 像素值 |
| 公开符号 / 依赖库 | ✅ 仅 `HmdDriverFactory` / 仅 `libc`、`libm`、`ld-linux`(libvulkan 在运行时 `dlopen`) |
| 在 Linux 版 SteamVR 2.17.10(Arch、KDE Wayland)上运行 v0.2.0 | ✅ 驱动加载、HMD 识别、控制器重映射 / ❌ 合成器因 `CannotDRMLeaseDisplay` 无法启动(null 驱动也一样)→ 由 v0.3.0 的直接模式解决 |
| **在 Linux 版 SteamVR 上运行 v0.3.0(直接模式)** | ❌ **未验证**。请用下文的 `linux_check.py` 确认 |

## 前提

- x86_64 Linux、Steam + SteamVR(Linux 原生版)
- g++ 9 及以上(或 clang)、CMake 3.20 及以上、Git、Python 3.10 及以上
- Vulkan 驱动(能运行 SteamVR 的环境中已安装)。Vulkan 头文件(`vulkan-headers` / `libvulkan-dev`)若不存在，会在 configure 时自动获取
- Ubuntu / Debian 示例：`sudo apt install build-essential cmake ninja-build git python3`
- Arch 示例：`sudo pacman -S base-devel cmake ninja git python vulkan-headers`

## 构建

```bash
git clone --recurse-submodules https://github.com/Nekomimi21336/VHMD_DevKit.git
cd VHMD_DevKit
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

输出：`build/driver/vhmd/bin/linux64/driver_vhmd.so`(+ `driver.vrdrivermanifest`、`resources/`)。

### glibc 注意事项

生成的 `.so` 要求**不低于构建环境的 glibc 版本**(在 Ubuntu 24.04 上构建则需要 glibc 2.38 及以上)。可以用 `tools/verify_env.sh` 的 `driver_needs_glibc` 确认。最可靠的做法是**在运行 SteamVR 的 PC 上自行构建**。

## 注册、卸载

```bash
tools/verify_env.sh            # 检查环境(只读)
tools/install_driver.sh        # vrpathreg.sh adddriver build/driver/vhmd
tools/uninstall_driver.sh --clean-settings   # 取消注册 + 从 SteamVR 设置中删除 vhmd 相关项(会先备份)
```

- `vrpathreg.sh` 位于 `<SteamVR>/bin/vrpathreg.sh`。SteamVR 的位置取自 `~/.config/openvr/openvrpaths.vrpath` 的 `runtime`，若没有则依次查找 `~/.steam/steam`、`~/.local/share/Steam`、Flatpak。
- 不会复制到 SteamVR 本体文件夹。
- **重新构建时请先退出 SteamVR。**

## 使用

```bash
python3 clients/python/examples/fullbody_sway.py --points 6
```

要查看 SteamVR 绘制到虚拟 HMD 上的画面：

```bash
python3 clients/python/examples/preview_viewer.py                 # 在窗口中显示(仅用标准库)
python3 clients/python/examples/preview_viewer.py --snapshot eyes.png
```

在自制工具中读取时，请使用 `vhmd_client.preview.PreviewReader`(格式见 `driver/src/util/preview_shm.h`：左右眼横向并排的 RGBA8，sRGB)。

| 设置(`vhmd_config.json` 的 `preview`) | 默认值 | 说明 |
|---|---|---|
| `enabled` | `true` | 是否写入共享内存 |
| `path` | `/dev/shm/vhmd_preview` | 写入位置 |
| `fps` | `15` | 写入的速率上限(与合成器的渲染速率无关) |
| `maxEyeWidth` | `960` | 单眼的最大宽度(缩小后写入) |
| `vulkanDevice` | `-1` | 所用 GPU 的编号。`-1` 表示第一个 GPU。**必须选择与 SteamVR 相同的 GPU**(在多 GPU 的 PC 上收不到画面时修改) |

预览处理即使失败也只会被禁用，不会影响 SteamVR 的渲染。

日志：`~/.steam/steam/logs/vrserver.txt`(位置取自 `openvrpaths.vrpath` 的 `log`)。以 `vhmd:` 开头的行来自本驱动。`config list` 响应中的 `display` 包含模式、present 次数、vsync 次数和预览状态。

## 实机确认(自动)

```bash
pip install openvr
python3 tests/integration/linux_check.py
```

从启动到退出 SteamVR 全部自动完成，并在 `tests/integration/results/linux_check_<日期时间>/` 中输出报告(`report.md`、`report.json`)、预览截图(`eyes.png`)和日志摘录。**只要把这个文件夹发过来，就能了解情况。** 确认内容：

| # | 确认项 | 预期 |
|---|---|---|
| 1 | 环境(发行版、会话类型、GPU、Vulkan、SteamVR 的构建版本和分支) | 信息 |
| 2 | 驱动的构建、注册 | ✅ |
| 3 | SteamVR 加载驱动并响应 UDP | ✅ |
| 4 | HMD 处于直接模式，vsync 正在运行 | ✅ |
| 5 | 合成器创建交换纹理，每秒 present 30 次以上 | ✅ 这是本次的重点 |
| 6 | `vrcompositor.txt` 中没有 `CannotDRMLeaseDisplay` 等错误 | ✅ |
| 7 | 预览中有持续更新的帧，并保存 `eyes.png` | ✅ |
| 8 | 常规集成测试(`run_integration.py`：枚举、姿态、输入、骨骼、负载) | ✅ |
| 9 | 退出 SteamVR 时执行驱动的退出处理 | ✅ |

使用 `--skip-integration` 可以跳过第 8 项(只想确认显示路径时)。

## 故障排查

| 症状 | 检查项 |
|---|---|
| 第 5 项为 0(没有 present) | `vrcompositor.txt` 的末尾。SteamVR 的版本(ALVR 可能是在测试版上确认运行的，在稳定版 2.17.10 上能否同样运行尚未确认)。`steamvr.vrsettings` 中是否残留以前通过 ALVR 等添加的启动选项或 `forcedDriver` |
| 有 present 但预览全黑 / 没有画面 | `report.json` 的 `display.preview.state`。多 GPU 时检查 `preview.vulkanDevice` |
| VRChat 无法启动 | 请在第 1~7 项全部为 ✅ 之后再尝试。查看 Proton 侧的日志(`PROTON_LOG=1`) |
| 想与真实 HMD 一起使用 | `"hmd": {"enabled": false}`(只添加控制器 / 追踪器) |

## 为支持 Linux 所做的修改

- **v0.3.0**：`driver/src/linux/`(`direct_mode.*`：直接模式组件和 vsync 时钟，`frame_capture.*`：共享图像的导入和缩小复制，`vk_loader.*`：libvulkan 的动态加载)、`util/preview_shm.h`、Python 的 `preview.py` / `preview_viewer.py`、`tests/linux/capture_selftest.cpp`、`tests/integration/linux_check.py`。
- `driver/driver_exports.map` + `-Wl,--version-script`：将 `HmdDriverFactory` 以外的符号设为非公开(因为 vrserver 会把多个驱动加载到同一进程中)。
- `-static-libstdc++ -static-libgcc`：即使 SteamVR 运行环境的 libstdc++ 较旧也能加载。
- `.gitattributes`：将 `*.sh` 固定为 LF。
- `tests/integration/steamvr_util.py`：通过 `/proc` 检测进程、用信号停止、通过 `vrstartup.sh` 启动、`~/.steam` / XDG 路径。
