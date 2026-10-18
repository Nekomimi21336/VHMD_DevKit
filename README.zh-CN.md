<div align="center">

# VHMD_DevKit

**用于开发的虚拟 HMD / 控制器 / 全身追踪器 SteamVR 驱动**

即使没有实体设备，也能把外部进程通过 UDP/JSON 发送的姿态和输入，以“仿佛真有实体设备”的方式呈现给 SteamVR。

[日本語](README.md) | [English](README.en.md) | **简体中文** | [한국어](README.ko.md)

</div>

---

## 目录

- [特点](#特点)
- [从 3 点开始，按需添加追踪器](#从-3-点开始按需添加追踪器)
- [快速开始(预编译二进制)](#快速开始预编译二进制)
- [从源码构建](#从源码构建)
- [使用方法(Python)](#使用方法python)
- [配置](#配置)
- [与真实 HMD 同时使用时](#与真实-hmd-同时使用时)
- [测试](#测试)
- [文档](#文档)
- [仓库结构](#仓库结构)
- [已验证环境](#已验证环境)
- [许可证](#许可证)

## 特点

- **以 3 点 VR 为基础**：HMD + 左右控制器从 SteamVR 启动时起始终存在
- **追踪器按需添加**：首次发送时才添加到 SteamVR，并从该姿态开始同步(最多 16 个)
- **简单的外部 API**：UDP + JSON(`127.0.0.1:39570`)。一个数据报即可批量更新所有设备。可选 VMT 兼容 OSC
- **完整的控制器输入**：扳机 / 握把 / 摇杆 / A/B/X/Y / 系统 / 菜单、振动回传、手指骨骼(弯曲值或直接指定骨骼)
- **控制器与手部追踪的切换**：通过 `hand_mode` 在不停止 SteamVR 的情况下切换(在 VRChat 中为按键操作 ⇄ 手部追踪操作)。发送的 ID 和内容保持不变
- **在 SteamVR 中易于区分**：追踪器的模型和状态图标显示部位(`L FOOT`、`WAIST` 等)和 ID(`TRACKER_1`)，控制器显示 `L` / `R` 和 ID。更改角色后显示随之更新
- **视线输入**(SteamVR 的眼动追踪组件)
- 支持 **Windows / Linux**。Linux 上采用直接模式(由驱动接收 SteamVR 渲染的画面)，可通过基于共享内存的查看器确认画面(v0.3.0，尚待在 Linux 版 SteamVR 上确认)
- **Python 客户端**(无依赖)及示例、可在 SteamVR 上运行的自动验证脚本
- **不冒充其他设备**：以自有控制器类型 `vhmd_*` 注册。现有应用的按键绑定通过 SteamVR 的重映射复用

> 用途为 VR 应用的开发与测试、头像 / IK / 全身追踪行为的验证、动作捕捉数据的导入。
> 请勿用于作弊、冒充他人设备，或在多人环境中进行不正当使用。

## 从 3 点开始，按需添加追踪器

| 配置 | 发送的设备 | 用途示例 |
|---|---|---|
| **3 点** | `hmd`, `left_hand`, `right_hand` | 普通 VR |
| **5 点** | + `left_foot`, `right_foot`(脚踝) | 脚部动作 |
| **6 点** | + `waist` | VRChat 的全身追踪 |
| 11 点 | + 膝、肘、胸(默认的 8 个追踪器全部) | 精细的动作捕捉 |
| 最多 19 点 | 在 `vhmd_config.json` 中添加追踪器(最多 16 个，也可指定肩、手腕、脚踝等) | |

追踪器在**发送的瞬间即被添加**(实测约 40 ms)，停止发送后会显示为断开。在重启 SteamVR 之前注册会一直保留，再次发送即可恢复。

## 快速开始(预编译二进制)

1. 从 [Releases](../../releases) 获取对应操作系统的压缩包，解压到**不会移动的位置**(解压后的文件夹会直接注册到 SteamVR)。
2. 在解压后的文件夹中进行注册(使用 SteamVR 的 `vrpathreg`。不会复制到 SteamVR 本体目录)。

   | 操作系统 | 压缩包 | 注册命令 |
   |---|---|---|
   | Windows | `VHMD_DevKit-<版>-win64.zip` | `powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1` |
   | Linux | `VHMD_DevKit-<版>-linux64.tar.gz` | `tools/install_driver.sh`(glibc 2.38 及以上) |

3. 启动 SteamVR，运行示例。

   ```bash
   python clients/python/examples/fullbody_sway.py --points 6
   ```

   `--points` 可取 `3` / `5` / `6` / `11`。

4. 卸载时使用 `tools/uninstall_driver.ps1`(Linux 为 `.sh`)。加上 `-CleanSettings`(Linux 为 `--clean-settings`)后，还会删除驱动写入 SteamVR 设置中的追踪器角色(会先备份)。

## 从源码构建

```bash
git clone --recurse-submodules https://github.com/Nekomimi21336/VHMD_DevKit.git
cd VHMD_DevKit
```

### Windows

前提：Visual Studio 2022(使用 C++ 的桌面开发)、CMake 3.20 及以上、Git、Steam + SteamVR、Python 3.10 及以上。

```bash
cmake --preset vs2022
cmake --build --preset vs2022-release
ctest --preset vs2022-release
powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1
```

- 输出为 SteamVR 可直接读取的 `build/driver/vhmd/`。
- **重新构建时请先退出 SteamVR**(正在加载的 DLL 无法被覆盖)。
- 如果只安装了 Visual Studio 2022，也可以使用 `cmake -S . -B build -A x64` → `cmake --build build --config Release`。如果安装了未包含 C++ 工作负载的新版 Visual Studio，CMake 会选中它而导致失败，因此预设中指定了 VS 2022。
- 如果没有 `third_party/openvr`(子模块)，会在 configure 时自动获取 OpenVR SDK v2.15.6。

### Linux

前提：g++ 9 及以上、CMake 3.20 及以上、Git、Python 3.10 及以上、Linux 版 SteamVR。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
tools/install_driver.sh
```

- Linux 版 SteamVR 无法以“绘制到桌面窗口”的方式启动，因此在 Linux 上驱动以直接模式运行，直接接收 SteamVR 的渲染结果。渲染出的画面可以用 `python3 clients/python/examples/preview_viewer.py` 查看。
- 实机确认使用 `python3 tests/integration/linux_check.py`(从启动到退出 SteamVR 全部自动完成，并输出报告)。

详见 [docs/zh-CN/linux.md](docs/zh-CN/linux.md)。

## 使用方法(Python)

```python
import sys; sys.path.insert(0, "clients/python")   # 或者：pip install -e clients/python
from vhmd_client import VHMDClient, math3d

with VHMDClient() as c:                            # 127.0.0.1:39570
    with c.batch():                                # 合并为 1 个数据报
        c.pose("hmd", (0, 1.7, 0), math3d.from_euler(yaw=30))
        c.pose("left_hand", (-0.2, 1.2, -0.3))
        c.pose("right_hand", (0.2, 1.2, -0.3))
        c.pose("left_foot", (-0.12, 0.08, 0))      # 首次发送时添加到 SteamVR
        c.pose("right_foot", (0.12, 0.08, 0))
    c.inputs("right_hand", {"trigger": 1.0, "a": True, "joystick_x": 0.5})
    c.skeleton_curl("left_hand", [0, 1, 1, 1, 1])  # 除拇指外全部握拳
    c.gaze(direction=(0.1, 0, -1))
    print(c.status())                              # 注册状态、接收速率、最后接收时间
```

其他语言只需通过 UDP 发送 JSON 即可。

```json
{ "type": "pose", "id": "left_foot", "pos": [-0.12, 0.08, 0.0], "rot": [1, 0, 0, 0] }
```

| `type` | 内容 |
|---|---|
| `pose` | 位置、旋转、速度 |
| `input` | 按键、扳机、摇杆 |
| `skeleton` | 手指(弯曲值或 31 根骨骼) |
| `gaze` | 视线 |
| `config` | `list` / `ping` / `add` / `set_role` / `reset` |

- 坐标系遵循 SteamVR(右手系、+Y 向上、-Z 向前、单位米)，旋转为 `[w, x, y, z]`。
- 非法值(NaN、超出范围、类型错误等)会被丢弃，驱动不会停止。
- 完整规范：[docs/zh-CN/protocol.md](docs/zh-CN/protocol.md)

## 配置

| 文件 | 内容 |
|---|---|
| `driver/resources/settings/vhmd_config.json` | 网络、更新周期、超时(`hold` / `invalid` / `disconnect`)、平滑、VMT 兼容、各设备的启用、角色、默认姿态、`trackersOnDemand` |
| `driver/resources/settings/default.vrsettings` | `driver_vhmd`(启用、通过 `configFile` 指定外部配置文件)、`vhmd_display`(窗口 / 分辨率 / FOV / 刷新率 / IPD) |

配置在 SteamVR 启动时读取。请编辑源码侧的文件并重新构建，或通过 `configFile` 指定外部文件。

**安全**：默认只在 `127.0.0.1` 上监听。如需从其他 PC 发送，请修改 `network.bind` 并显式设置 `"allowRemote": true`。由于没有认证机制，请仅在可信网络中使用，并自行配置防火墙。

## 与真实 HMD 同时使用时

- 本驱动的 HMD 始终会被注册，因此如果与真实 HMD 的驱动同时启用，哪一个成为活动设备取决于具体环境。
- 在用实体设备玩之前，请先取消注册，或在 `vhmd_config.json` 中设置 `"hmd": {"enabled": false}`，切换为**仅添加控制器 / 追踪器的模式**(尚未在连接真实 HMD 的环境中验证)。
- 也可以在 SteamVR 的“启动/关闭 → 管理加载项”中禁用 `vhmd`。

## 测试

| 类型 | 命令 | 内容 |
|---|---|---|
| C++ 单元测试 | `ctest --preset vs2022-release` | 37 项(解析器、坐标变换、配置、超时、路由器、UDP) |
| Python 单元测试 | `python -m pytest tests/python` | 13 项(客户端、预览读取) |
| Linux 捕获自测 | `ctest`(Linux) | 用真实 Vulkan 确认共享图像 → 共享内存(无需 SteamVR) |
| Linux 实机确认 | `python3 tests/integration/linux_check.py` | 直接模式的显示路径 + 集成测试，输出报告 |
| SteamVR 集成测试 | `python tests/integration/run_integration.py` | 31 项(枚举、按需添加、误差、延迟、输入、骨骼、手部模式切换、非法数据包、负载、退出处理) |
| 反复启动/退出 | `python tests/integration/stress_restart.py --cycles 30` | 每次都检查驱动的退出处理以及是否崩溃 |

集成测试需要 `pip install openvr`。如果环境中存在启动后会重启 SteamVR 的叠加层应用，请加上 `--stop-relaunchers`(Steam 本体和游戏不受影响)。

## 文档

| 内容 | 日本語 | English | 简体中文 | 한국어 |
|---|---|---|---|---|
| README | [README.md](README.md) | [README.en.md](README.en.md) | 本页 | [README.ko.md](README.ko.md) |
| 协议规范 | [protocol](docs/protocol.md) | [protocol](docs/en/protocol.md) | [protocol](docs/zh-CN/protocol.md) | [protocol](docs/ko/protocol.md) |
| 架构、扩展方法 | [architecture](docs/architecture.md) | [architecture](docs/en/architecture.md) | [architecture](docs/zh-CN/architecture.md) | [architecture](docs/ko/architecture.md) |
| Linux | [linux](docs/linux.md) | [linux](docs/en/linux.md) | [linux](docs/zh-CN/linux.md) | [linux](docs/ko/linux.md) |
| 验证步骤与结果 | [verification](docs/verification.md) | — | — | — |
| 决策、未处理事项记录 | [decisions](docs/decisions.md) | — | — | — |

验证记录和决策记录属于开发记录，因此仅有日文版。

## 仓库结构

```
CMakeLists.txt / CMakePresets.json   构建配置(获取 OpenVR SDK、按平台输出)
driver/
  driver.vrdrivermanifest
  resources/            设置、输入配置文件、重映射、旧版绑定、图标
  src/
    main.cpp            HmdDriverFactory(唯一的公开符号)
    device_provider.*   设备管理、接收 / 更新线程、按需添加
    device_base.*       所有设备通用
    hmd_device.*        HMD、虚拟显示、视线
    linux/              Linux 的直接模式、帧捕获、Vulkan 的动态加载
    controller_device.* 控制器、振动、骨骼
    tracker_device.*    追踪器、角色
    math/ protocol/ config/ state/ net/ util/   不依赖 OpenVR 的核心(单元测试对象)
clients/python/         vhmd_client 包与 examples/
tools/                  注册 / 卸载 / 环境检查(Windows：.ps1、Linux：.sh)、发布包制作
tests/                  unit/(C++)、python/、integration/(SteamVR)
docs/                   文档(见上表)
third_party/            OpenVR SDK(子模块 v2.15.6)、nlohmann/json 3.11.3
```

## 已验证环境

| 项目 | 版本 |
|---|---|
| SteamVR | 2.17.10(Windows 11、Linux：Ubuntu 26.04 + NVIDIA RTX 4000 Ada) |
| OpenVR SDK | v2.15.6 |
| Windows 编译器 | MSVC 14.44(Visual Studio 2022 Build Tools 17.14)、CMake 4.3 |
| Linux 编译器 | gcc 13.3(Ubuntu 24.04 / WSL2)、gcc 15.2(Ubuntu 26.04)、CMake 3.28 / 4.2 |
| Python | 3.12(3.10 及以上)、验证用 pyopenvr 2.12 |

详细结果：[docs/verification.md](docs/verification.md)(日文)。在 Linux 版 SteamVR 上，v0.3.0 的直接模式已在真实 GPU(NVIDIA RTX 4000 Ada)上确认：合成器以 90 fps 渲染、集成测试 28/28 通过、在有 Steam 客户端并启用 SteamVR Home 的环境中正常退出、通过 Proton 运行的 VRChat 可渲染并跟随控制器。在 GTX 1050 上 SteamVR 合成器会在启动后不久因 GPU 异常停止([docs/zh-CN/linux.md](docs/zh-CN/linux.md))。登录 VRChat 后的操作(虚拟形象、全身校准)以及与实体 HMD 并用尚未验证。

## 许可证

**BSD 3-Clause License** — Copyright (c) 2026, Nekomimi21336([LICENSE](LICENSE))

| | |
|---|---|
| ✅ 商业使用 | 允许 |
| ✅ 修改、创建衍生作品 | 允许 |
| ✅ 再分发(源码 / 预编译二进制) | 允许 |
| 📄 **义务** | 再分发时，须包含版权声明、条件、免责声明([LICENSE](LICENSE) 全文)：源码形式原样保留，二进制形式则包含在随附文档等中 |
| 🚫 禁止 | 未经许可，使用版权持有者的名字为衍生作品背书或做宣传 |
| ⚠ 无担保 | 按“原样”提供，版权持有者对因使用造成的损害不承担责任 |

以上摘要仅供参考。正式条款以 [LICENSE](LICENSE) 的英文原文为准。

署名示例(用于文档或“关于”界面等)：

```
This product includes VHMD_DevKit
Copyright (c) 2026, Nekomimi21336 — BSD 3-Clause License
https://github.com/Nekomimi21336/VHMD_DevKit
```

随附的第三方组件(OpenVR SDK：BSD 3-Clause / nlohmann/json：MIT)的许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。再分发时请一并包含(发布压缩包中已附带)。
