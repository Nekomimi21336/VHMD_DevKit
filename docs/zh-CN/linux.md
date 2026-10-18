[日本語](../linux.md) | [English](../en/linux.md) | **简体中文** | [한국어](../ko/linux.md) — [返回 README](../../README.zh-CN.md)

# 在 Linux 版 SteamVR 上使用

本文介绍让 Linux 原生 SteamVR 加载 `driver_vhmd.so` 的步骤。通过 Proton 运行的 Windows 游戏(VRChat 等)同样连接到 Linux 版 SteamVR，因此需要本驱动。发送端的协议和 Python 客户端与 Windows 通用。

## 状态(截至 2026-10-18)

| 项目 | 状态 |
|---|---|
| Linux 构建 (gcc 13.3, Ubuntu 24.04 / WSL2) | ✅ 0 个警告(`-Wall -Wextra -Wpedantic`) |
| C++ 单元测试 36 项 | ✅ 全部通过(含 UDP 回环) |
| 公开符号 | ✅ 仅 `HmdDriverFactory`(链接器版本脚本) |
| 依赖库 | ✅ 仅 `libc`, `libm`, `ld-linux`(libstdc++ / libgcc 为静态链接) |
| `tools/*.sh` | ✅ 已在模拟的 SteamVR 环境中确认 install / uninstall / `--clean-settings` |
| 集成测试的 Linux 支持 (`tests/integration/steamvr_util.py`) | ⚠ 仅在 WSL 上确认了加载和进程检测 |
| **在真实 Linux 版 SteamVR 上的运行** | ❌ **未验证**(这台开发 PC 上没有 Linux 版 SteamVR) |

## 前提

- x86_64 Linux、Steam + SteamVR(Linux 原生版)
- g++ 9 及以上(或 clang)、CMake 3.20 及以上、Git、Python 3.10 及以上
- Ubuntu / Debian 示例：`sudo apt install build-essential cmake ninja-build git python3`

## 构建

```bash
git clone --recurse-submodules <本仓库>
cd VirtualHMD
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

输出：`build/driver/vhmd/bin/linux64/driver_vhmd.so`(+ `driver.vrdrivermanifest`, `resources/`)。

### glibc 注意事项

生成的 `.so` 要求**不低于构建环境的 glibc 版本**(在 Ubuntu 24.04 上构建则需要 glibc 2.38 及以上)。可以用 `tools/verify_env.sh` 的 `driver_needs_glibc` 确认。最可靠的做法是**在运行 SteamVR 的 PC 上自行构建**。如果还要分发给较旧的发行版，请在旧环境(或 Steam Runtime SDK 的容器)中构建。

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
python3 clients/python/examples/fullbody_sway.py
```

日志：`~/.steam/steam/logs/vrserver.txt`(位置取自 `openvrpaths.vrpath` 的 `log`)。以 `vhmd:` 开头的行来自本驱动。

## 最大风险：虚拟显示

HMD 采用通过 `IVRDisplayComponent` 显示为“桌面上的窗口”的方式(已在 Windows 上确认可用，与 OpenVR 示例 `simplehmd` 相同)。**Linux 版 SteamVR 的合成器能否以这种方式启动尚不清楚**。无法启动时的迹象与对策：

| 迹象 | 对策 |
|---|---|
| SteamVR 提示“找不到头显 / 合成器无法启动” | 先设置 `hmd.enabled: false`(与真实 HMD 配合使用的追踪器/控制器专用模式)，只使用其他功能 |
| 在没有真实 HMD 的 PC 上也想让 HMD 为虚拟设备 | 实现 `IVRVirtualDisplay`(替换 `HmdDevice::GetComponent` 返回的组件。参见 `docs/architecture.md` 的扩展点) |
| `vrcompositor` 的日志中出现 Vulkan / 显示相关错误 | 让 `vhmd_display` 的 `windowWidth/Height` 不超出屏幕范围；如果是 Wayland，也可尝试 X11 会话 |

## 实机验证检查清单

请填写结果并同步到 `docs/verification.md`。

| # | 步骤 | 预期结果 | 结果 |
|---|---|---|---|
| L1 | `tools/verify_env.sh` | SteamVR 的路径、glibc ≥ `driver_needs_glibc` | 未验证 |
| L2 | `tools/install_driver.sh` → 启动 SteamVR | `vrserver.txt` 中出现 `Loaded server driver vhmd` 和 `vhmd: listening on udp://127.0.0.1:39570` | 未验证 |
| L3 | 同一日志 | `Active HMD set to vhmd.VHMD-HMD-0`(未连接真实 HMD 时)。合成器是否启动(见上述风险) | 未验证 |
| L4 | `pip install openvr` → `python3 tests/integration/run_integration.py` | 与 Windows 相同的 29 项。`--stop-relaunchers` 会结束自动启动的叠加层，仅在需要时使用 | 未验证 |
| L5 | `python3 clients/python/examples/fullbody_sway.py` + SteamVR 的“头显视图” | 视角和设备会移动 | 未验证 |
| L6 | 通过 Proton 以 VR 模式启动 VRChat | HMD、手、追踪器均被反映 | 未验证 |
| L7 | `hmd.enabled: false` + 真实 HMD | 与真实 HMD 并列添加控制器/追踪器 | 未验证 |
| L8 | 退出 SteamVR | 输出 `vhmd: shutting down` 且 vrserver 正常退出(不挂起) | 未验证 |

## 为支持 Linux 所做的修改

- `driver/driver_exports.map` + `-Wl,--version-script`：将 `HmdDriverFactory` 以外的符号设为非公开(vrserver 会把多个驱动加载到同一进程中，以此防止符号冲突)。
- `-static-libstdc++ -static-libgcc`：即使 SteamVR 运行环境的 libstdc++ 较旧也能加载。通过 `-Wl,--no-undefined` 在构建时检测未解析的符号。
- 修复了 4 个 gcc 特有的警告(初始化、检查 OSC bundle 长度读取的返回值、对临时对象的引用)。
- `.gitattributes`：将 `*.sh` 固定为 LF(以免被 Windows 的 `core.autocrlf` 破坏)。
- `tests/integration/steamvr_util.py`：通过 `/proc` 检测进程、用信号停止、通过 `vrstartup.sh` 启动、`~/.steam` / XDG 路径。OpenVR 客户端的检测以加载了 `vrclient.so` 的进程为准，Steam 本体及 `steamapps` 下的进程(含 Proton 游戏)不在此列。
