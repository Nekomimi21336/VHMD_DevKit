<div align="center">

# VHMD_DevKit

**개발용 가상 HMD / 컨트롤러 / 풀 트래커 SteamVR 드라이버**

실제 장비가 없어도, 외부 프로세스에서 UDP/JSON으로 보낸 포즈와 입력을 SteamVR에 "실제 장비가 있는 것처럼" 보여 줍니다.

[日本語](README.md) | [English](README.en.md) | [简体中文](README.zh-CN.md) | **한국어**

</div>

---

## 목차

- [특징](#특징)
- [3점에서 시작해 필요한 만큼 트래커 추가하기](#3점에서-시작해-필요한-만큼-트래커-추가하기)
- [시작하기 (빌드된 바이너리)](#시작하기-빌드된-바이너리)
- [소스에서 빌드](#소스에서-빌드)
- [사용법 (Python)](#사용법-python)
- [설정](#설정)
- [실제 HMD와 함께 사용할 때](#실제-hmd와-함께-사용할-때)
- [테스트](#테스트)
- [문서](#문서)
- [저장소 구성](#저장소-구성)
- [동작 확인 환경](#동작-확인-환경)
- [라이선스](#라이선스)

## 특징

- **3점 VR이 기본**: HMD + 좌우 컨트롤러는 SteamVR 시작 시점부터 항상 존재
- **트래커는 온디맨드**: 처음 보낸 시점에 SteamVR에 추가되며, 그 포즈에서 동기화된 상태로 시작(최대 16대)
- **간단한 외부 API**: UDP + JSON(`127.0.0.1:39570`). 데이터그램 1개로 모든 디바이스를 한 번에 갱신 가능. 선택적으로 VMT 호환 OSC
- **컨트롤러 입력 일체**: 트리거 / 그립 / 스틱 / A·B·X·Y / 시스템 / 메뉴, 진동 피드백 반환, 손가락 스켈레톤(굽힘 값 또는 본 직접 지정)
- **시선 입력**(SteamVR의 아이 트래킹 컴포넌트)
- **Windows / Linux** 지원. Linux는 다이렉트 모드(SteamVR이 그린 영상을 드라이버가 받는 방식)로 동작하며, 영상은 공유 메모리를 통한 뷰어로 확인 가능(v0.3.0. Linux판 SteamVR에서의 확인 대기 중)
- **Python 클라이언트**(의존성 없음)와 예제, SteamVR 위에서 동작하는 자동 검증 스크립트
- **위장 없음**: 독자적인 컨트롤러 타입 `vhmd_*`로 등록. 기존 앱의 바인딩은 SteamVR의 리매핑으로 재사용

> 용도는 VR 앱 개발·테스트, 아바타 / IK / 풀 트래킹 동작 검증, 모션 캡처 데이터 입력입니다.
> 치트, 타인 디바이스의 위장, 대인 환경에서의 부정 사용에는 사용하지 마십시오.

## 3점에서 시작해 필요한 만큼 트래커 추가하기

| 구성 | 보내는 디바이스 | 용도 예시 |
|---|---|---|
| **3점** | `hmd`, `left_hand`, `right_hand` | 일반적인 VR |
| **5점** | + `left_foot`, `right_foot`(발목) | 발의 움직임 |
| **6점** | + `waist` | VRChat의 풀바디 트래킹 |
| 11점 | + 무릎·팔꿈치·가슴(기본 트래커 8대 전부) | 상세한 모션 캡처 |
| 최대 19점 | `vhmd_config.json`에 트래커 추가(최대 16대, 어깨·손목·발목 등도 지정 가능) | |

트래커는 **보낸 순간 추가**되며(실측 약 40 ms), 송신이 멈추면 연결 끊김으로 표시됩니다. SteamVR을 재시작할 때까지 등록은 유지되고, 다시 보내면 복귀합니다.

## 시작하기 (빌드된 바이너리)

1. [Releases](../../releases)에서 OS용 아카이브를 받아 **옮기지 않을 위치**에 압축을 풉니다(압축을 푼 폴더가 그대로 SteamVR에 등록됩니다).
2. 압축을 푼 폴더에서 등록합니다(SteamVR의 `vrpathreg` 사용. SteamVR 본체로 복사하지 않습니다).

   | OS | 아카이브 | 등록 명령 |
   |---|---|---|
   | Windows | `VHMD_DevKit-<버전>-win64.zip` | `powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1` |
   | Linux | `VHMD_DevKit-<버전>-linux64.tar.gz` | `tools/install_driver.sh`(glibc 2.38 이상) |

3. SteamVR을 시작하고 예제를 실행합니다.

   ```bash
   python clients/python/examples/fullbody_sway.py --points 6
   ```

   `--points`는 `3` / `5` / `6` / `11`.

4. 해제할 때는 `tools/uninstall_driver.ps1`(Linux는 `.sh`). `-CleanSettings`(Linux는 `--clean-settings`)를 붙이면 드라이버가 SteamVR 설정에 기록한 트래커 롤도 백업과 함께 삭제합니다.

## 소스에서 빌드

```bash
git clone --recurse-submodules https://github.com/Nekomimi21336/VHMD_DevKit.git
cd VHMD_DevKit
```

### Windows

전제 조건: Visual Studio 2022(C++를 사용한 데스크톱 개발), CMake 3.20 이상, Git, Steam + SteamVR, Python 3.10 이상.

```bash
cmake --preset vs2022
cmake --build --preset vs2022-release
ctest --preset vs2022-release
powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1
```

- 출력은 SteamVR이 그대로 읽을 수 있는 `build/driver/vhmd/`입니다.
- **다시 빌드할 때는 SteamVR을 종료하십시오**(로드 중인 DLL은 덮어쓸 수 없습니다).
- Visual Studio 2022만 설치되어 있다면 `cmake -S . -B build -A x64` → `cmake --build build --config Release`로도 됩니다. 더 새로운 Visual Studio가 C++ 워크로드 없이 설치되어 있으면 CMake가 그쪽을 선택해 실패하므로, 프리셋에서 VS 2022를 지정하고 있습니다.
- `third_party/openvr`(서브모듈)가 없으면 configure 시에 OpenVR SDK v2.15.6을 자동으로 가져옵니다.

### Linux

전제 조건: g++ 9 이상, CMake 3.20 이상, Git, Python 3.10 이상, Linux판 SteamVR.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
tools/install_driver.sh
```

- Linux판 SteamVR은 "데스크톱 창에 그리는" 방식으로 시작할 수 없으므로, Linux에서는 드라이버가 SteamVR의 렌더링 결과를 직접 받는 다이렉트 모드로 동작합니다. 렌더링된 영상은 `python3 clients/python/examples/preview_viewer.py`로 볼 수 있습니다.
- 실제 장비에서의 확인은 `python3 tests/integration/linux_check.py`(SteamVR 시작부터 종료까지 자동, 리포트 출력).

자세한 내용은 [docs/ko/linux.md](docs/ko/linux.md).

## 사용법 (Python)

```python
import sys; sys.path.insert(0, "clients/python")   # 또는: pip install -e clients/python
from vhmd_client import VHMDClient, math3d

with VHMDClient() as c:                            # 127.0.0.1:39570
    with c.batch():                                # 데이터그램 1개로 묶음
        c.pose("hmd", (0, 1.7, 0), math3d.from_euler(yaw=30))
        c.pose("left_hand", (-0.2, 1.2, -0.3))
        c.pose("right_hand", (0.2, 1.2, -0.3))
        c.pose("left_foot", (-0.12, 0.08, 0))      # 처음 보낸 시점에 SteamVR에 추가됨
        c.pose("right_foot", (0.12, 0.08, 0))
    c.inputs("right_hand", {"trigger": 1.0, "a": True, "joystick_x": 0.5})
    c.skeleton_curl("left_hand", [0, 1, 1, 1, 1])  # 엄지 이외의 손가락을 쥠
    c.gaze(direction=(0.1, 0, -1))
    print(c.status())                              # 등록 상태, 수신 레이트, 마지막 수신 시각
```

다른 언어에서는 JSON을 UDP로 보내기만 하면 됩니다.

```json
{ "type": "pose", "id": "left_foot", "pos": [-0.12, 0.08, 0.0], "rot": [1, 0, 0, 0] }
```

| `type` | 내용 |
|---|---|
| `pose` | 위치·회전·속도 |
| `input` | 버튼·트리거·스틱 |
| `skeleton` | 손가락(굽힘 값 또는 31개 본) |
| `gaze` | 시선 |
| `config` | `list` / `ping` / `add` / `set_role` / `reset` |

- 좌표계는 SteamVR 기준(오른손 좌표계, +Y 위, -Z 전방, 미터), 회전은 `[w, x, y, z]`.
- 잘못된 값(NaN·범위 밖·타입 불일치 등)은 폐기되며, 드라이버는 멈추지 않습니다.
- 전체 사양: [docs/ko/protocol.md](docs/ko/protocol.md)

## 설정

| 파일 | 내용 |
|---|---|
| `driver/resources/settings/vhmd_config.json` | 네트워크, 갱신 주기, 타임아웃(`hold` / `invalid` / `disconnect`), 스무딩, VMT 호환, 각 디바이스의 활성화·롤·기본 포즈, `trackersOnDemand` |
| `driver/resources/settings/default.vrsettings` | `driver_vhmd`(활성화, `configFile`로 외부 설정 파일 지정), `vhmd_display`(창 / 해상도 / FOV / 주사율 / IPD) |

설정은 SteamVR 시작 시에 읽힙니다. 소스 쪽을 편집해 다시 빌드하거나, `configFile`로 외부 파일을 지정하십시오.

**보안**: 기본적으로 `127.0.0.1`에서만 수신 대기합니다. 다른 PC에서 보내려면 `network.bind`를 변경하고 `"allowRemote": true`를 명시하십시오. 인증이 없으므로 신뢰할 수 있는 네트워크에서만 사용하고, 방화벽은 각자 설정하십시오.

## 실제 HMD와 함께 사용할 때

- 이 드라이버의 HMD는 항상 등록되므로, 실제 HMD의 드라이버와 동시에 활성화되어 있으면 어느 쪽이 활성 HMD가 될지는 환경에 따라 다릅니다.
- 실제 장비로 플레이하기 전에 등록을 해제하거나, `vhmd_config.json`에서 `"hmd": {"enabled": false}`로 설정해 **컨트롤러 / 트래커만 추가하는 모드**로 사용하십시오(실제 HMD가 연결된 환경에서의 확인은 미실시).
- SteamVR의 "시작/종료 → 애드온 관리"에서 `vhmd`를 비활성화할 수도 있습니다.

## 테스트

| 종류 | 명령 | 내용 |
|---|---|---|
| C++ 단위 테스트 | `ctest --preset vs2022-release` | 37건(파서, 좌표 변환, 설정, 타임아웃, 라우터, UDP) |
| Python 단위 테스트 | `python -m pytest tests/python` | 13건(클라이언트, 프리뷰 읽기) |
| Linux 캡처 셀프 테스트 | `ctest`(Linux) | 실제 Vulkan으로 공유 이미지 → 공유 메모리를 확인(SteamVR 불필요) |
| Linux 실제 장비 확인 | `python3 tests/integration/linux_check.py` | 다이렉트 모드의 표시 경로 + 통합 테스트, 리포트 출력 |
| SteamVR 통합 테스트 | `python tests/integration/run_integration.py` | 29개 항목(열거, 온디맨드 추가, 오차, 지연, 입력, 스켈레톤, 잘못된 패킷, 부하, 종료 처리) |
| 시작/종료 반복 | `python tests/integration/stress_restart.py --cycles 30` | 매번 드라이버의 종료 처리와 크래시 여부를 확인 |

통합 테스트에는 `pip install openvr`이 필요합니다. 시작 후 SteamVR을 재시작해 버리는 오버레이 앱이 있는 환경에서는 `--stop-relaunchers`를 붙이십시오(Steam 본체와 게임은 대상 외).

## 문서

| 내용 | 日本語 | English | 简体中文 | 한국어 |
|---|---|---|---|---|
| README | [README.md](README.md) | [README.en.md](README.en.md) | [README.zh-CN.md](README.zh-CN.md) | 이 페이지 |
| 프로토콜 사양 | [protocol](docs/protocol.md) | [protocol](docs/en/protocol.md) | [protocol](docs/zh-CN/protocol.md) | [protocol](docs/ko/protocol.md) |
| 아키텍처·확장 방법 | [architecture](docs/architecture.md) | [architecture](docs/en/architecture.md) | [architecture](docs/zh-CN/architecture.md) | [architecture](docs/ko/architecture.md) |
| Linux | [linux](docs/linux.md) | [linux](docs/en/linux.md) | [linux](docs/zh-CN/linux.md) | [linux](docs/ko/linux.md) |
| 검증 절차와 결과 | [verification](docs/verification.md) (일본어) | — | — | — |
| 판단·미대응 사항 기록 | [decisions](docs/decisions.md) (일본어) | — | — | — |

검증 기록과 판단 기록은 개발 기록이므로 일본어로만 제공됩니다.

## 저장소 구성

```
CMakeLists.txt / CMakePresets.json   빌드 설정(OpenVR SDK 가져오기, 플랫폼별 출력)
driver/
  driver.vrdrivermanifest
  resources/            설정, 입력 프로파일, 리매핑, 레거시 바인딩, 아이콘
  src/
    main.cpp            HmdDriverFactory(유일한 공개 심볼)
    device_provider.*   디바이스 관리, 수신 / 갱신 스레드, 온디맨드 추가
    device_base.*       모든 디바이스 공통
    hmd_device.*        HMD, 가상 디스플레이, 시선
    linux/              Linux의 다이렉트 모드, 프레임 캡처, Vulkan 동적 로드
    controller_device.* 컨트롤러, 진동, 스켈레톤
    tracker_device.*    트래커, 롤
    math/ protocol/ config/ state/ net/ util/   OpenVR 비의존 코어(단위 테스트 대상)
clients/python/         vhmd_client 패키지와 examples/
tools/                  등록 / 해제 / 환경 확인(Windows: .ps1, Linux: .sh), 배포물 생성
tests/                  unit/(C++), python/, integration/(SteamVR)
docs/                   문서(위의 표)
third_party/            OpenVR SDK(서브모듈 v2.15.6), nlohmann/json 3.11.3
```

## 동작 확인 환경

| 항목 | 버전 |
|---|---|
| SteamVR | 2.17.10(Windows 11) |
| OpenVR SDK | v2.15.6 |
| Windows 컴파일러 | MSVC 14.44(Visual Studio 2022 Build Tools 17.14), CMake 4.3 |
| Linux 컴파일러 | gcc 13.3(Ubuntu 24.04 / WSL2), CMake 3.28, Vulkan은 Mesa llvmpipe로 셀프 테스트 |
| Python | 3.12(3.10 이상), 검증용으로 pyopenvr 2.12 |

결과 상세: [docs/verification.md](docs/verification.md) (일본어). VRChat에서의 동작 확인(GUI 조작)과, Linux판 SteamVR에서의 다이렉트 모드(v0.3.0) 동작은 미검증입니다. v0.2.0에서는 Linux판 SteamVR 2.17.10에서 드라이버 로드까지 확인했으며, 컴포지터가 시작되지 않는 문제가 발견되었습니다([docs/ko/linux.md](docs/ko/linux.md)).

## 라이선스

**BSD 3-Clause License** — Copyright (c) 2026, Nekomimi21336([LICENSE](LICENSE))

| | |
|---|---|
| ✅ 상업적 이용 | 가능 |
| ✅ 수정·파생물 작성 | 가능 |
| ✅ 재배포(소스 / 빌드된 바이너리) | 가능 |
| 📄 **의무** | 재배포할 때는 저작권 표시·조건·면책 문구([LICENSE](LICENSE) 전문)를, 소스라면 그대로, 바이너리라면 동봉 문서 등에 포함할 것 |
| 🚫 금지 | 저작권자의 이름을 허가 없이 파생물의 추천·홍보에 사용하는 것 |
| ⚠ 무보증 | 있는 그대로 제공되며, 이용으로 인한 손해에 대해 저작권자는 책임을 지지 않습니다 |

위 요약은 참고용입니다. 정식 조건은 [LICENSE](LICENSE)의 영문이 우선합니다.

크레딧 표기 예시(문서나 About 화면 등에):

```
This product includes VHMD_DevKit
Copyright (c) 2026, Nekomimi21336 — BSD 3-Clause License
https://github.com/Nekomimi21336/VHMD_DevKit
```

동봉된 서드파티(OpenVR SDK: BSD 3-Clause / nlohmann/json: MIT)의 라이선스는 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)에 있습니다. 재배포 시에는 이것도 포함하십시오(배포 아카이브에는 이미 동봉되어 있음).
