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

## 상태(2026-10-18 기준)

| 항목 | 상태 |
|---|---|
| Linux 빌드 (gcc 13.3, Ubuntu 24.04 / WSL2) | ✅ 경고 0(`-Wall -Wextra -Wpedantic`) |
| C++ 단위 테스트 37건 | ✅ 모두 성공(ASan / TSan에서도 문제 없음) |
| 프레임 캡처 셀프 테스트(`tests/linux/capture_selftest.cpp`) | ✅ 실제 Vulkan(Mesa llvmpipe)으로 fd 임포트 → 축소 → 공유 메모리 → 픽셀 값까지 확인 |
| 공개 심볼 / 의존 라이브러리 | ✅ `HmdDriverFactory`만 / `libc`, `libm`, `ld-linux`만(libvulkan은 실행 시 `dlopen`) |
| v0.2.0을 Linux판 SteamVR 2.17.10(Arch, KDE Wayland)에서 실행 | ✅ 드라이버 로드, HMD 인식, 컨트롤러 리맵 / ❌ 컴포지터가 `CannotDRMLeaseDisplay`로 시작되지 않음(null 드라이버도 마찬가지) → v0.3.0의 다이렉트 모드로 대응 |
| **v0.3.0(다이렉트 모드)을 Linux판 SteamVR에서 실행** | ❌ **미검증**. 아래의 `linux_check.py`로 확인해 주십시오 |

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

`--skip-integration`으로 8을 생략할 수 있습니다(표시 경로만 확인하고 싶을 때).

## 잘 되지 않을 때

| 증상 | 확인할 사항 |
|---|---|
| 5가 0(present가 오지 않음) | `vrcompositor.txt`의 끝부분. SteamVR 버전(ALVR은 베타 버전에서 동작을 확인했을 가능성이 있으며, 안정 버전 2.17.10에서 똑같이 동작하는지는 미확인). `steamvr.vrsettings`에 예전에 ALVR 등으로 넣은 시작 옵션이나 `forcedDriver`가 남아 있지 않은지 |
| present는 오지만 프리뷰가 새까맣거나 오지 않음 | `report.json`의 `display.preview.state`. GPU가 여러 개라면 `preview.vulkanDevice` |
| VRChat이 시작되지 않음 | 1~7이 모두 ✅가 된 뒤에 시도해 주십시오. Proton 측 로그(`PROTON_LOG=1`) |
| 실제 HMD와 함께 사용하고 싶음 | `"hmd": {"enabled": false}`(컨트롤러 / 트래커만 추가) |

## Linux 지원을 위해 넣은 변경 사항

- **v0.3.0**: `driver/src/linux/`(`direct_mode.*`: 다이렉트 모드 컴포넌트와 vsync 클럭, `frame_capture.*`: 공유 이미지 임포트와 축소 복사, `vk_loader.*`: libvulkan 동적 로드), `util/preview_shm.h`, Python의 `preview.py` / `preview_viewer.py`, `tests/linux/capture_selftest.cpp`, `tests/integration/linux_check.py`.
- `driver/driver_exports.map` + `-Wl,--version-script`: `HmdDriverFactory` 이외를 비공개로 합니다(vrserver는 여러 드라이버를 같은 프로세스에 로드하므로).
- `-static-libstdc++ -static-libgcc`: SteamVR 실행 환경의 libstdc++가 오래되어도 로드할 수 있도록 합니다.
- `.gitattributes`: `*.sh`를 LF로 고정합니다.
- `tests/integration/steamvr_util.py`: `/proc`을 이용한 프로세스 검출, 시그널을 통한 정지, `vrstartup.sh`를 통한 시작, `~/.steam` / XDG 경로.
