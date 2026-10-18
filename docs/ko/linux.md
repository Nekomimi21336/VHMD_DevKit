[日本語](../linux.md) | [English](../en/linux.md) | [简体中文](../zh-CN/linux.md) | **한국어** — [README로 돌아가기](../../README.ko.md)

# Linux판 SteamVR에서의 사용법

Linux 네이티브 SteamVR에 `driver_vhmd.so`를 로드시키는 절차입니다. Proton으로 동작하는 Windows 게임(VRChat 등)도 Linux판 SteamVR에 접속하므로 이 드라이버가 필요합니다. 송신 측 프로토콜과 Python 클라이언트는 Windows와 공통입니다.

## 상태(2026-10-18 기준)

| 항목 | 상태 |
|---|---|
| Linux 빌드 (gcc 13.3, Ubuntu 24.04 / WSL2) | ✅ 경고 0(`-Wall -Wextra -Wpedantic`) |
| C++ 단위 테스트 36건 | ✅ 모두 성공(UDP 루프백 포함) |
| 공개 심볼 | ✅ `HmdDriverFactory`만(링커 버전 스크립트) |
| 의존 라이브러리 | ✅ `libc`, `libm`, `ld-linux`만(libstdc++ / libgcc는 정적 링크) |
| `tools/*.sh` | ✅ 가짜 SteamVR 환경에서 install / uninstall / `--clean-settings`를 확인 |
| 통합 테스트의 Linux 지원 (`tests/integration/steamvr_util.py`) | ⚠ WSL에서 로드와 프로세스 검출만 확인 |
| **실제 Linux판 SteamVR에서의 동작** | ❌ **미검증**(이 개발 PC에는 Linux판 SteamVR이 없음) |

## 전제 조건

- x86_64 Linux, Steam + SteamVR(Linux 네이티브판)
- g++ 9 이상(또는 clang), CMake 3.20 이상, Git, Python 3.10 이상
- Ubuntu / Debian 예: `sudo apt install build-essential cmake ninja-build git python3`

## 빌드

```bash
git clone --recurse-submodules <이 저장소>
cd VirtualHMD
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

출력: `build/driver/vhmd/bin/linux64/driver_vhmd.so`(+ `driver.vrdrivermanifest`, `resources/`).

### glibc 주의 사항

완성된 `.so`는 **빌드한 환경의 glibc 이상**을 요구합니다(Ubuntu 24.04에서 빌드하면 glibc 2.38 이상). `tools/verify_env.sh`의 `driver_needs_glibc`로 확인할 수 있습니다. **SteamVR을 실행할 PC 자체에서 빌드하는 것**이 확실합니다. 오래된 배포판에도 배포하려면 오래된 환경(또는 Steam Runtime SDK 컨테이너)에서 빌드하십시오.

## 등록·해제

```bash
tools/verify_env.sh            # 환경 확인(읽기 전용)
tools/install_driver.sh        # vrpathreg.sh adddriver build/driver/vhmd
tools/uninstall_driver.sh --clean-settings   # 등록 해제 + SteamVR 설정에서 vhmd 항목 삭제(백업 포함)
```

- `vrpathreg.sh`는 `<SteamVR>/bin/vrpathreg.sh`. SteamVR의 위치는 `~/.config/openvr/openvrpaths.vrpath`의 `runtime`, 없으면 `~/.steam/steam`, `~/.local/share/Steam`, Flatpak 순서로 찾습니다.
- SteamVR 본체 폴더로 복사하지 않습니다.
- **다시 빌드할 때는 SteamVR을 종료하십시오.**

## 사용하기

```bash
python3 clients/python/examples/fullbody_sway.py
```

로그: `~/.steam/steam/logs/vrserver.txt`(위치는 `openvrpaths.vrpath`의 `log`). `vhmd:`로 시작하는 줄이 이 드라이버의 로그입니다.

## 최대 리스크: 가상 디스플레이

HMD는 `IVRDisplayComponent`로 "데스크톱 위의 창"으로 표시하는 방식(Windows에서 동작 확인 완료, OpenVR 샘플 `simplehmd`와 동일)입니다. **Linux판 SteamVR의 컴포지터가 이 방식으로 시작되는지는 불명**입니다. 시작되지 않는 경우의 징후와 대처:

| 징후 | 대처 |
|---|---|
| SteamVR이 "헤드셋을 찾을 수 없음 / 컴포지터가 시작되지 않음" | 우선 `hmd.enabled: false`(실제 HMD와 함께 사용하는 트래커/컨트롤러 전용 모드)로 다른 기능만 사용 |
| 실제 HMD가 없는 PC에서 HMD도 가상으로 하고 싶음 | `IVRVirtualDisplay`를 구현(`HmdDevice::GetComponent`에서 반환하는 컴포넌트를 교체. `docs/architecture.md`의 확장 포인트 참조) |
| `vrcompositor` 로그에 Vulkan / 디스플레이 오류 | `vhmd_display`의 `windowWidth/Height`를 화면 안에 들어가게 조정, Wayland라면 X11 세션에서도 시도 |

## 실제 장비 확인 체크리스트

결과를 기입하여 `docs/verification.md`(일본어)에 반영해 주십시오.

| # | 절차 | 기대 결과 | 결과 |
|---|---|---|---|
| L1 | `tools/verify_env.sh` | SteamVR 경로, glibc ≥ `driver_needs_glibc` | 미검증 |
| L2 | `tools/install_driver.sh` → SteamVR 시작 | `vrserver.txt`에 `Loaded server driver vhmd`와 `vhmd: listening on udp://127.0.0.1:39570` | 미검증 |
| L3 | 같은 로그 | `Active HMD set to vhmd.VHMD-HMD-0`(실제 HMD 미연결 시). 컴포지터가 시작되는지(위 리스크) | 미검증 |
| L4 | `pip install openvr` → `python3 tests/integration/run_integration.py` | Windows와 같은 29개 항목. `--stop-relaunchers`는 자동 시작 오버레이를 종료시키므로 필요할 때만 | 미검증 |
| L5 | `python3 clients/python/examples/fullbody_sway.py` + SteamVR의 "헤드셋 뷰" | 시점과 디바이스가 움직임 | 미검증 |
| L6 | Proton으로 VRChat을 VR 모드로 시작 | HMD·손·트래커가 반영됨 | 미검증 |
| L7 | `hmd.enabled: false` + 실제 HMD | 실제 HMD와 함께 컨트롤러/트래커가 추가됨 | 미검증 |
| L8 | SteamVR 종료 | `vhmd: shutting down`이 출력되고 vrserver가 종료됨(행이 걸리지 않음) | 미검증 |

## Linux 지원을 위해 넣은 변경 사항

- `driver/driver_exports.map` + `-Wl,--version-script`: `HmdDriverFactory` 이외를 비공개(vrserver는 여러 드라이버를 같은 프로세스에 로드하므로 심볼 충돌을 방지).
- `-static-libstdc++ -static-libgcc`: SteamVR 실행 환경의 libstdc++가 오래되어도 로드할 수 있도록. `-Wl,--no-undefined`로 미해결 심볼을 빌드 시에 검출.
- gcc 고유 경고 4건을 수정(초기화, OSC 번들 길이 읽기의 반환값 확인, 임시 객체에 대한 참조).
- `.gitattributes`: `*.sh`를 LF로 고정(Windows의 `core.autocrlf`로 깨지지 않도록).
- `tests/integration/steamvr_util.py`: `/proc`을 이용한 프로세스 검출, 시그널을 통한 정지, `vrstartup.sh` 시작, `~/.steam` / XDG 경로. OpenVR 클라이언트 검출은 `vrclient.so`를 로드한 프로세스로 판단하며, Steam 본체·`steamapps` 하위(Proton 게임 포함)는 대상 외.
