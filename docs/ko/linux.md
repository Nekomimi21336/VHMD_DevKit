[日本語](../linux.md) | [English](../en/linux.md) | [简体中文](../zh-CN/linux.md) | **한국어** — [README로 돌아가기](../../README.ko.md)

# Linux판 SteamVR에서의 사용법

Linux 네이티브 SteamVR에 `driver_vhmd.so`를 로드시키는 절차입니다. Proton으로 동작하는 Windows 게임(VRChat 등)도 Linux판 SteamVR에 접속하므로 이 드라이버가 필요합니다. 송신 측 프로토콜과 Python 클라이언트는 Windows와 공통입니다.

## 구조: Linux에서는 "다이렉트 모드"로 동작

Linux판 SteamVR의 컴포지터는 Windows처럼 "데스크톱 위의 창에 그리는" 것이 불가능합니다. 다이렉트 모드(DRM 리스)로 얻을 수 있는 출력 대상이 없으면 `VRInitError_Compositor_CannotDRMLeaseDisplay`로 시작을 중단합니다. SteamVR에 포함된 null 드라이버에서도 마찬가지이며, Valve도 알려진 문제로 다루고 있습니다([SteamVR-for-Linux#889](https://github.com/ValveSoftware/SteamVR-for-Linux/issues/889)).

그래서 v0.3.0부터 Linux의 가상 HMD는 **`IVRDriverDirectModeComponent`를** 구현합니다.

```
앱 ──▶ vrcompositor ──(공유 Vulkan 이미지 / dma-buf)──▶ driver_vhmd.so
                                                         ├─ vsync를 직접 통지(기본 90 Hz)
                                                         └─ 최신 프레임을 /dev/shm/vhmd_preview로(선택)
```

- 실제 디스플레이도 DRM 리스도 필요 없습니다. 좌우 눈의 이미지는 드라이버가 확보한 공유 Vulkan 이미지에 그려지고 `Present`로 전달됩니다(`IVRIPCResourceManagerClient`의 Linux 전용 API).
- ALVR의 Linux 드라이버(2026-10 시점의 master)와 같은 방식입니다. 코드는 가져다 쓰지 않고 OpenVR 헤더에 따라 구현했습니다.
- Windows는 기존과 같이 데스크톱 창 방식입니다(`display.mode`: `auto` / `window` / `direct`).

## 상태(2026-10-19 기준)

| 항목 | 상태 |
|---|---|
| Linux 빌드 (gcc 13.3, Ubuntu 24.04 / WSL2) | ✅ 경고 0(`-Wall -Wextra -Wpedantic`) |
| C++ 단위 테스트 37건 | ✅ 모두 성공(ASan / TSan에서도 문제 없음) |
| 프레임 캡처 셀프 테스트(`tests/linux/capture_selftest.cpp`) | ✅ 실제 Vulkan(Mesa llvmpipe)으로 fd 임포트 → 축소 → 공유 메모리 → 픽셀 값까지 확인 |
| 공개 심볼 / 의존 라이브러리 | ✅ `HmdDriverFactory`만 / `libc`, `libm`, `ld-linux`만(libvulkan은 실행 시 `dlopen`) |
| v0.2.0을 Linux판 SteamVR 2.17.10(Arch, KDE Wayland)에서 실행 | ✅ 드라이버 로드, HMD 인식, 컨트롤러 리맵 / ❌ 컴포지터가 `CannotDRMLeaseDisplay`로 시작되지 않음(null 드라이버도 마찬가지) → v0.3.0의 다이렉트 모드로 대응 |
| v0.3.0을 WSL2 위의 Linux판 SteamVR 2.17.10(소프트웨어 Vulkan)에서 실행 | ✅ 드라이버 로드, 다이렉트 모드, 통합 테스트 21/23, 컴포지터가 `Headset is using driver direct mode`까지 도달 / ❌ llvmpipe가 타임라인 세마포어 외부 가져오기를 지원하지 않아 렌더러 초기화 실패([verification.md](../verification.md), 일본어) |
| v0.3.0을 실제 GPU의 Linux판 SteamVR 2.17.10에서 실행(클라우드 GPU: NVIDIA RTX 4000 Ada, 드라이버 595, Ubuntu 26.04, gcc 15.2로 빌드, 디스플레이 없음 + Xvfb) | ✅ `linux_check.py` 전 항목 합격: 컴포지터가 스왑 텍스처(한쪽 눈 2160×2400)를 만들고 **90 fps로 present**, `CannotDRMLeaseDisplay` 없음, 프리뷰에 SteamVR 영상, 통합 테스트 28/28(스켈레톤·입력 포함). 2회 연속 합격 |
| 데스크톱에 상당하는 환경(NVIDIA 실제 X 서버 + Steam 클라이언트 + SteamVR Home 활성) | ✅ `linux_check.py` 14/14 합격. vrmonitor가 동작하며 SteamVR의 정상 종료와 드라이버 종료 처리(`vhmd: shutting down`, 약 2초)까지 확인. 클라우드 GPU(RTX 4000 Ada)에서 확인. Proxmox VM의 GTX 1050(2 GB, Pascal, 드라이버 580)에서도 14/14였으나 **시작 후 5~60초 뒤 vrcompositor가 GPU 예외(`Xid 13: 3D WIDTH ZT Violation` → `VK_ERROR_DEVICE_LOST`)로 렌더링을 멈춤**. 클라이언트·프리뷰·SteamVR Home이 모두 없어도 재현되며 드라이버 측 원인은 발견되지 않음(GTX 1050은 SteamVR 최소 요구 사양 GTX 970 / 1060 미만) |
| Proton을 통한 VRChat(Proton Experimental) | ✅ 실행되어 가상 HMD의 양쪽 눈에 VR 영상(로그인 화면)을 렌더링. VRChat의 Oculus Touch 바인딩이 가상 컨트롤러에 적용되고 게임 내 손이 보낸 자세를 따라감. 로그인 후에도 아바타의 머리·손·손가락 추종, 이동, 버튼, 메뉴, 컨트롤러 ⇄ 핸드 트래킹 전환(VRChat을 멈추지 않고)을 확인. 풀바디 보정도 핸드 모드에서 완료했으며 발 트래커로 아바타의 다리가 움직이는 것을 확인. 컨트롤러 모드에서는 메뉴 버튼을 클릭할 수 없음(Windows도 동일. 메뉴 조작은 핸드 모드로). **가상 머신 안에서는 VRChat의 Easy Anti-Cheat가 `Cannot run under Virtual Machine`으로 실행을 거부합니다**(클라우드 KVM에서는 실행됨. VM 감지를 숨기는 설정은 안티치트 회피에 해당하므로 하지 않았습니다) |

## 전제 조건

- x86_64 Linux, Steam + SteamVR(Linux 네이티브판)
- g++ 9 이상(또는 clang), CMake 3.20 이상, Git, Python 3.10 이상
- Vulkan 드라이버(SteamVR이 동작하는 환경이라면 설치되어 있습니다). Vulkan 헤더(`vulkan-headers` / `libvulkan-dev`)는 없으면 configure 시에 자동으로 가져옵니다
- Ubuntu / Debian 예: `sudo apt install build-essential cmake ninja-build git python3`
- Arch 예: `sudo pacman -S base-devel cmake ninja git python vulkan-headers`

## 빌드

```bash
git clone --recurse-submodules https://github.com/Nekomimi21336/VHMD_DevKit.git
cd VHMD_DevKit
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

출력: `build/driver/vhmd/bin/linux64/driver_vhmd.so`(+ `driver.vrdrivermanifest`, `resources/`).

### glibc 주의 사항

완성된 `.so`는 **빌드한 환경의 glibc 이상**을 요구합니다(Ubuntu 24.04에서 빌드하면 glibc 2.38 이상). `tools/verify_env.sh`의 `driver_needs_glibc`로 확인할 수 있습니다. **SteamVR을 실행할 PC 자체에서 빌드하는 것**이 확실합니다.

## 등록 및 해제

```bash
tools/verify_env.sh            # 환경 확인(읽기 전용)
tools/install_driver.sh        # vrpathreg.sh adddriver build/driver/vhmd
tools/uninstall_driver.sh --clean-settings   # 등록 해제 + SteamVR 설정에서 vhmd 항목 삭제(백업 포함)
```

- `vrpathreg.sh`는 `<SteamVR>/bin/vrpathreg.sh`입니다. SteamVR의 위치는 `~/.config/openvr/openvrpaths.vrpath`의 `runtime`에서, 없으면 `~/.steam/steam`, `~/.local/share/Steam`, Flatpak 순서로 찾습니다.
- SteamVR 본체 폴더로 복사하지 않습니다.
- **다시 빌드할 때는 SteamVR을 종료하십시오.**

## 사용하기

```bash
python3 clients/python/examples/fullbody_sway.py --points 6
```

SteamVR이 가상 HMD에 그리고 있는 영상을 보려면:

```bash
python3 clients/python/examples/preview_viewer.py                 # 창으로 표시(표준 라이브러리만 사용)
python3 clients/python/examples/preview_viewer.py --snapshot eyes.png
```

직접 만든 도구에서 읽을 때는 `vhmd_client.preview.PreviewReader`를 사용합니다(형식: `driver/src/util/preview_shm.h`. 좌우 눈을 가로로 나란히 놓은 RGBA8, sRGB).

| 설정(`vhmd_config.json`의 `preview`) | 기본값 | 내용 |
|---|---|---|
| `enabled` | `true` | 공유 메모리로 내보내기 |
| `path` | `/dev/shm/vhmd_preview` | 내보낼 위치 |
| `fps` | `15` | 내보내기의 상한 레이트(컴포지터의 렌더링 레이트와는 무관) |
| `maxEyeWidth` | `960` | 한쪽 눈당 최대 폭(축소해서 내보냄) |
| `vulkanDevice` | `-1` | 사용할 GPU 번호. `-1`은 첫 번째 GPU. **SteamVR과 같은 GPU를 선택해야 합니다**(GPU가 여러 개인 PC에서 영상이 오지 않을 때 변경) |

프리뷰 처리에서 실패해도 프리뷰가 비활성화될 뿐이며, SteamVR의 렌더링에는 영향을 주지 않습니다.

로그: `~/.steam/steam/logs/vrserver.txt`(위치는 `openvrpaths.vrpath`의 `log`). `vhmd:`로 시작하는 줄이 이 드라이버의 로그입니다. `config list` 응답의 `display`에 모드, present 횟수, vsync 횟수, 프리뷰 상태가 들어 있습니다.

## 실제 장비에서의 확인(자동)

```bash
pip install openvr
python3 tests/integration/linux_check.py
```

SteamVR의 시작부터 종료까지를 자동으로 수행하고, `tests/integration/results/linux_check_<일시>/`에 리포트(`report.md`, `report.json`), 프리뷰 스냅숏(`eyes.png`), 로그 발췌를 출력합니다. **이 폴더를 보내 주시면 상황을 파악할 수 있습니다.** 확인하는 내용:

| # | 확인 | 기대 결과 |
|---|---|---|
| 1 | 환경(배포판, 세션 종류, GPU, Vulkan, SteamVR 빌드와 브랜치) | 정보 |
| 2 | 드라이버 빌드 및 등록 | ✅ |
| 3 | SteamVR이 드라이버를 로드하고 UDP에 응답 | ✅ |
| 4 | HMD가 다이렉트 모드이며 vsync가 돌고 있음 | ✅ |
| 5 | 컴포지터가 스왑 텍스처를 만들고 초당 30회 이상 present | ✅ 이것이 이번 확인의 핵심 |
| 6 | `vrcompositor.txt`에 `CannotDRMLeaseDisplay` 등이 없음 | ✅ |
| 7 | 프리뷰에 진행되는 프레임이 있고 `eyes.png`를 저장 | ✅ |
| 8 | 일반 통합 테스트(`run_integration.py`: 열거, 포즈, 입력, 스켈레톤, 부하) | ✅ |
| 9 | SteamVR 종료 시 드라이버의 종료 처리가 실행됨 | ✅ |
| 10 | 종료 전(기본 60초 대기 후)에도 컴포지터가 계속 present하고 있음(`--hold`로 초 변경) | ✅ |

`--skip-integration`으로 8을 생략할 수 있습니다(표시 경로만 확인하고 싶을 때).

## 잘 되지 않을 때

| 증상 | 확인할 사항 |
|---|---|
| 5가 0(present가 오지 않음) | `vrcompositor.txt`의 끝부분. SteamVR 버전(ALVR은 베타 버전에서 동작을 확인했을 가능성이 있으며, 안정 버전 2.17.10에서 똑같이 동작하는지는 미확인). `steamvr.vrsettings`에 예전에 ALVR 등으로 넣은 시작 옵션이나 `forcedDriver`가 남아 있지 않은지 |
| present는 오지만 프리뷰가 새까맣거나 오지 않음 | `report.json`의 `display.preview.state`. GPU가 여러 개라면 `preview.vulkanDevice` |
| VRChat이 시작되지 않음 | 1~7이 모두 ✅가 된 뒤에 시도해 주십시오. Proton 측 로그(`PROTON_LOG=1`) |
| 실제 HMD와 함께 사용하고 싶음 | `"hmd": {"enabled": false}`(컨트롤러 / 트래커만 추가) |
| 디스플레이가 없는 머신(클라우드 GPU 등)에서 SteamVR Home이 시작과 실패를 반복하고 Scene 앱이 `AnotherAppLaunching`이 됨 | `steamvr.vrsettings`의 `steamvr`에 `"enableHomeApp": false` |
| 디스플레이가 없는 머신에서 "SteamVR shutdown"이 SKIP | vrserver는 vrmonitor(SteamVR 창)의 지시로만 종료되며 SIGTERM도 무시합니다. Xvfb에서는 vrmonitor 자체가 시작 시 충돌하므로 테스트가 SteamVR을 강제 종료합니다(드라이버 종료 처리는 실행되지 않음). 데스크톱에서는 정상적으로 종료됩니다 |
| Steam 클라이언트에서 처음 SteamVR을 실행하면 관리자 비밀번호를 요구하며 멈춤 | SteamVR 설정(`vrsetup.sh`)이 컴포지터에 `CAP_SYS_NICE`를 부여하려는 것입니다. 비밀번호를 입력하거나 `sudo setcap CAP_SYS_NICE=eip <SteamVR>/bin/linux64/vrcompositor-launcher` |
| VM 안에서 VRChat이 `Cannot run under Virtual Machine` | VRChat의 Easy Anti-Cheat 사양입니다. 실제 하드웨어의 Linux에서 실행해 주십시오 |
| 처음에는 합격하지만 "compositor still presenting at the end"가 FAIL이고 `dmesg`에 `NVRM: Xid 13` | GPU 예외로 컴포지터가 멈춘 것입니다. GTX 1050(Pascal, 드라이버 580)에서 재현. SteamVR 최소 요구 사양(GTX 970 / 1060) 이상의 GPU로 시도해 주십시오 |
| VM, WSL 등 GPU가 없는 환경 | 소프트웨어 Vulkan(llvmpipe)에서는 컴포지터가 렌더러 초기화 단계에서 `does not support timeline import`로 멈춥니다. 실제 GPU가 필요합니다 |

## 디스플레이가 없는 머신(클라우드 GPU 등)에서 실행하기

Steam 클라이언트도 디스플레이도 없는 Linux에서 검증할 때의 절차입니다. 일반 데스크톱에서는 필요 없습니다.

1. GPU 드라이버(NVIDIA라면 `nvidia-driver-5xx`)를 설치하고 `vulkaninfo --summary`에 GPU가 나오는지 확인
2. `steamcmd`로 SteamVR(250820, 로그인 필요)과 Steam Linux Runtime 3.0 sniper(1628350)를 설치. `~/.steam/steam` → Steam 폴더, `~/.steam/sdk64/steamclient.so` → `steamcmd/linux64/steamclient.so` 심볼릭 링크
3. SteamVR이 호스트 라이브러리로 동작하기 위한 추가 패키지(Ubuntu): `libsdl2-2.0-0 libpulse0 libpipewire-0.3-0t64`
4. Ubuntu 24.04 이후는 비특권 사용자 네임스페이스가 제한되어 있으므로 `sysctl kernel.apparmor_restrict_unprivileged_userns=0`(pressure-vessel이 사용)
5. `Xvfb :1`을 시작하고 `DISPLAY=:1`. 사용자 세션 버스(`loginctl enable-linger <user>`로 `/run/user/<uid>/bus`)를 `DBUS_SESSION_BUS_ADDRESS`에 설정
6. Steam 클라이언트 대신 실행 서비스를 띄움: `VALVE_SKIP_RUNTIME_SAFETY=1 SteamLinuxRuntime_sniper/pressure-vessel/bin/steam-runtime-launcher-service --alongside-steam --session &`
7. `steamvr.vrsettings`에 `"enableHomeApp": false`, `tools/install_driver.sh`, `python3 tests/integration/linux_check.py`

## Linux 지원을 위해 넣은 변경 사항

- **v0.3.0**: `driver/src/linux/`(`direct_mode.*`: 다이렉트 모드 컴포넌트와 vsync 클럭, `frame_capture.*`: 공유 이미지 임포트와 축소 복사, `vk_loader.*`: libvulkan 동적 로드), `util/preview_shm.h`, Python의 `preview.py` / `preview_viewer.py`, `tests/linux/capture_selftest.cpp`, `tests/integration/linux_check.py`.
- `driver/driver_exports.map` + `-Wl,--version-script`: `HmdDriverFactory` 이외를 비공개로 합니다(vrserver는 여러 드라이버를 같은 프로세스에 로드하므로).
- `-static-libstdc++ -static-libgcc`: SteamVR 실행 환경의 libstdc++가 오래되어도 로드할 수 있도록 합니다.
- `.gitattributes`: `*.sh`를 LF로 고정합니다.
- `tests/integration/steamvr_util.py`: `/proc`을 이용한 프로세스 검출, 시그널을 통한 정지, `vrstartup.sh`를 통한 시작, `~/.steam` / XDG 경로.
