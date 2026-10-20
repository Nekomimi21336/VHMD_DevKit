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
- [키보드와 마우스로 조작하기(Windows)](#키보드와-마우스로-조작하기windows)
- [창 없이 사용하기 / FPS 낮추기(Windows)](#창-없이-사용하기--fps-낮추기windows)
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
- **간단한 외부 API**: UDP + JSON(`127.0.0.1:39570`). 데이터그램 1개로 모든 디바이스를 한 번에 갱신 가능. 선택적으로 VMT 호환 OSC(Joint / Follow / 스켈레톤 / 룸 행렬 / 출력 메시지를 포함한 VMT 0.15의 API)
- **컨트롤러 입력 일체**: 트리거 / 그립(force 지원) / 스틱 / 트랙패드 / 섬레스트 / A·B·X·Y / 시스템 / 메뉴, 진동 피드백 반환, 손가락 스켈레톤(굽힘 값 또는 본 직접 지정). 트래커에도 버튼·트리거·트랙패드
- **디바이스 상태**: HMD 착용 센서(SteamVR의 대기 상태와 OpenXR의 `XR_EXT_user_presence`에 반영), 배터리 잔량과 충전 중 표시, 자세의 가속도와 샘플 지연(`poseTimeOffset`)
- **컨트롤러와 핸드 트래킹 전환**: `hand_mode`로 SteamVR을 멈추지 않고 전환(VRChat에서는 버튼 조작 ⇄ 핸드 트래킹 조작). 보내는 ID와 내용은 그대로
- **SteamVR에서 구분하기 쉬운 표시**: 트래커 모델과 상태 아이콘에 부위(`L FOOT`, `WAIST` 등)와 ID(`TRACKER_1`), 컨트롤러에 `L` / `R`와 ID를 표시. 롤을 바꾸면 표시도 따라감
- **시선 입력**(SteamVR의 아이 트래킹 컴포넌트. OpenVR과 OpenXR 양쪽 모두에서 앱까지 전달되는 것을 확인)
- **Windows / Linux** 지원. Linux는 다이렉트 모드(SteamVR이 그린 영상을 드라이버가 받는 방식)로 동작하며, 실제 GPU의 Linux판 SteamVR에서 동작 확인 완료
- **창 없이 저부하로 실행 가능**: Windows에서도 다이렉트 모드를 선택할 수 있으며, 프리뷰 창 표시 여부와 프레임 레이트(1~240 Hz, 실행 중 변경 가능)를 지정 가능. 영상은 Python에서 가져올 수 있음(머신러닝·자동화용)
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

   등록하기 전에 SteamVR 설정 파일(`openvrpaths.vrpath`, `steamvr.vrsettings`, `chaperone_info.vrchap`)과 등록된 드라이버 목록을 기록합니다(Windows: `%LOCALAPPDATA%\VHMD_DevKit\history`, Linux: `~/.local/state/vhmd_devkit/history`).

3. SteamVR을 시작하고 예제를 실행합니다.

   ```bash
   python clients/python/examples/fullbody_sway.py --points 6
   ```

   `--points`는 `3` / `5` / `6` / `11`.

4. 해제할 때는 `tools/uninstall_driver.ps1`(Linux는 `.sh`). 등록 해제와 함께 드라이버가 만든 파일(라벨이 붙은 모델과 아이콘)과 SteamVR 설정의 vhmd 항목(트래커 롤 등)을 삭제하고, 설치 후 바뀐 그 밖의 설정을 목록으로 보여 줍니다(그 설정은 건드리지 않습니다).
   - `-Restore`(Linux는 `--restore`): 설치 때 기록한 상태로 그대로 되돌립니다. SteamVR을 닫은 뒤 실행하세요. 해제 직전 상태도 기록되므로 `-Restore -Record <기록>`으로 다시 되돌릴 수 있습니다.
   - `-List`(`--list`): 기록 목록. `-KeepSettings` / `-KeepGeneratedFiles`(`--keep-settings` / `--keep-generated-files`)로 삭제를 생략할 수 있습니다.

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

## 키보드와 마우스로 조작하기(Windows)

```powershell
python -m vhmd_client.fps        # clients/python 에서 실행, 또는 pip install -e clients/python
```

FPS 게임처럼 마우스로 시점, WASD 로 이동하며 HMD 와 컨트롤러를 직접 움직일 수 있습니다. 각 디바이스의 위치와 각도를 표에서 확인·입력할 수 있고, 위에서 본 그림에서 디바이스를 드래그해 옮길 수 있습니다. 다이렉트 모드(`display.mode = "direct"`)에서는 HMD 의 시점도 도구 안에 표시합니다(부드럽게 보려면 `preview.fps` 를 30 으로). 자세는 전용 스레드에서 일정한 간격(초당 120 회)으로 보냅니다.

| 조작 | 내용 |
|---|---|
| F1 / 버튼 | 마우스를 잡고 조작 시작(Esc 로 해제) |
| 마우스 / WASD / Space·C | 시점 / 이동 / 위아래(Shift 빠르게, Alt 천천히, Q/E 고개 기울이기) |
| F2 / F3 | 왼손 / 오른손을 마우스로 이동(휠로 앞뒤) |
| 왼쪽·오른쪽·가운데 클릭 | 오른쪽 트리거·왼쪽 트리거·오른쪽 그립(G 양쪽 그립, 1~4 A/B/X/Y, M 메뉴, 화살표 오른쪽 스틱) |
| L | WASD 를 왼쪽 스틱으로 전송(앱 안에서의 이동) |
| 표 | 값을 입력하고 Enter. 위아래 키나 휠로 1 cm / 1° 씩(Shift 로 10 배) |
| 기준 | 손은 "머리"(머리를 따라감), 트래커는 "몸"(머리의 바닥 위치와 방향을 따라감)이 기본. "고정"은 그 자리에 머무름 |
| 일시 정지 / SteamVR 자세 이어받기 | 전송을 멈추고 자동 조작 프로그램에 맡김 / SteamVR 의 현재 자세에서 수동 조작을 재개 |
| Python 으로 복사 | 현재 자세를 `c.pose(...)` 줄로 클립보드에 복사 |
| Raw 마우스 입력 | 켜면(기본) Windows 마우스 가속을 무시하고 마우스 이동량을 그대로 시점에 사용 |
| 움직임 예측 | VHMD 모드에서는 기본 꺼짐, 실제 HMD 를 따라가는 Pico 모드에서는 기본 켜짐(실제 HMD 로 돌아다녔을 때 발 트래커의 최대 오차 28 → 10 mm). 켜면 속도도 보내 SteamVR 이 전송 사이를 보간(관성 같은 느낌이 날 수 있음) |
| 시점 표시 | 한쪽 눈 16:9(기본) / 양쪽 눈 / 한쪽 눈(전체) 중 선택. 창 크기에 맞춰 확대·축소. 크게 보려면 `preview.maxEyeWidth` 를 1200 이상으로(기본 960 에서는 렌더 크기에 따라 한쪽 눈이 540 px 까지 줄어듦) |

- "SteamVR 상의 값" 열과 그림의 원은 앱에서 실제로 보이는 자세입니다(pyopenvr 필요: `pip install openvr`). 실제 HMD 등 다른 드라이버의 디바이스는 회색으로 표시합니다.
- VHMD 의 HMD 를 끄고 실제 HMD 와 함께 쓸 때는 머리가 실제 HMD 의 자세가 되어 트래커가 착용자를 따라갑니다.

## 창 없이 사용하기 / FPS 낮추기(Windows)

`vhmd_config.json`의 `display`를 바꾸면, SteamVR이 데스크톱에 창을 열지 않고 드라이버가 영상을 직접 받는 **다이렉트 모드**가 됩니다(Linux와 같은 방식). 카메라 라이브러리처럼 프리뷰 창을 띄울지 선택할 수 있고, 영상은 Python에서 꺼낼 수 있습니다.

```jsonc
"display": { "mode": "direct", "refreshHz": 30, "previewWindow": true }
```

| 설정 | 내용 |
|---|---|
| `mode: "direct"` | 다이렉트 모드(기본 `auto`는 Windows에서 창 모드) |
| `refreshHz` | 가상 디스플레이의 프레임 레이트(1~240, 기본 90). 머신러닝이나 자동화라면 30이나 15로 충분하며 SteamVR과 앱의 부하가 줄어듭니다. 실행 중에도 변경 가능 |
| `previewWindow` | 시작 시 프리뷰 창(좌우 눈 영상)을 표시. 창을 닫아도 SteamVR은 멈추지 않음 |
| `preview.windowFps` | 프리뷰 창의 갱신 레이트(기본 30) |

실행 중 전환(`VHMDClient`):

```python
c.refresh_rate(30)        # 프레임 레이트를 30으로(SteamVR을 멈추지 않고. 인수 없이 호출하면 현재 값)
c.preview_window(True)    # 프리뷰 창 표시 / False로 숨김
c.preview(True)           # 영상을 공유 메모리에 출력(꺼내는 쪽이 호출. 멈출 때는 False)
from vhmd_client.preview import PreviewReader
frame = PreviewReader().read()   # 좌우 눈을 가로로 나란히 놓은 RGBA. frame.save_png("eyes.png") 등
```

SteamVR 컴포지터는 1~240 Hz의 어떤 값에도 정확히 따릅니다. 다만 10 Hz 미만으로 해도 앱은 12~15 fps 아래로 내려가지 않습니다(SteamVR이 대기를 중단하기 때문). 앱의 부하를 줄이는 실질적인 하한은 15 Hz 전후입니다.

`python clients/python/examples/preview_viewer.py`로도 영상을 확인할 수 있습니다. 보는 사람이 없는 동안(창을 숨김, 영상 요청 없음)은 영상 복사를 전혀 하지 않습니다.

부하 비교(Windows 11, RX 7900 XTX, 매 프레임 렌더링하는 테스트 앱, CPU는 코어 1개 대비):

| 조건 | 앱 fps | CPU: compositor + vrserver + 앱 | GPU(3D): compositor + vrserver + 앱 |
|---|---|---|---|
| 창 모드 90 Hz(이전) | 66.6 | 32.1 + 5.1 + 1.6 = **38.8 %** | 측정 불가 + 0 + 3.7 % |
| 다이렉트 90 Hz(창 없음) | 89.9 | 10.0 + 6.9 + 1.5 = **18.4 %** | 6.8 + 0 + 2.7 = 9.5 % |
| 다이렉트 90 Hz + 프리뷰 창 | 89.9 | 10.7 + 8.0 + 2.0 = 20.7 % | 7.1 + 1.5 + 2.5 = 11.1 % |
| 다이렉트 90 Hz + Python에 15 fps로 영상 | 89.9 | 11.2 + 6.6 + 1.8 = 19.6 % | 6.8 + 0.6 + 2.6 = 10.0 % |
| 다이렉트 60 Hz | 59.9 | 7.2 + 7.8 + 1.1 = 16.1 % | 4.6 + 0 + 1.8 = 6.4 % |
| 다이렉트 30 Hz | 30.0 | 5.5 + 7.2 + 0.0 = **12.7 %** | 2.8 + 0 + 0.9 = **3.7 %** |
| 다이렉트 15 Hz | 15.0 | 3.1 + 6.0 + 0.3 = **9.4 %** | 1.3 + 0 + 0.6 = **1.9 %** |

창 모드는 앱이 66 fps에서 멈추고 compositor의 CPU가 높습니다. 다이렉트 모드는 90 fps를 내면서 전체 부하가 약 절반, 30 Hz로 하면 약 3분의 1입니다(무거운 앱일수록 fps를 낮춘 효과가 큽니다). 상세: [docs/verification.md](docs/verification.md)(일본어)

## 설정

| 파일 | 내용 |
|---|---|
| `driver/resources/settings/vhmd_config.json` | 네트워크, 갱신 주기, 타임아웃(`hold` / `invalid` / `disconnect`), 스무딩, VMT 호환, 각 디바이스의 활성화·롤·기본 포즈, `trackersOnDemand` |
| `driver/resources/settings/default.vrsettings` | `driver_vhmd`(활성화, `configFile`로 외부 설정 파일 지정), `vhmd_display`(창 / 해상도 / FOV / 주사율 / IPD) |

설정은 SteamVR 시작 시에 읽힙니다. 소스 쪽을 편집해 다시 빌드하거나, `configFile`로 외부 파일을 지정하십시오.

**보안**: 기본적으로 `127.0.0.1`에서만 수신 대기합니다. 다른 PC에서 보내려면 `network.bind`를 변경하고 `"allowRemote": true`를 명시하십시오. 인증이 없으므로 신뢰할 수 있는 네트워크에서만 사용하고, 방화벽은 각자 설정하십시오.

## 실제 HMD와 함께 사용할 때

실제 장비의 머리와 양손(예: Pico 4 Ultra + PICO Connect)에 VHMD의 트래커를 더해 사용할 수 있습니다(Pico 4 Ultra로 확인).

```json
"hmd":         { "enabled": false },
"controllers": { "enabled": false }
```

- 실제 HMD의 드라이버와 VHMD는 SteamVR 안에서 나란히 동작합니다(서로 직접 통신하지 않음). 트래커의 자세는 PC의 송신 프로그램에서 UDP로 보냅니다.
- `activateMultipleDrivers`를 바꾸지 않아도 VHMD의 디바이스가 실제 HMD와 함께 활성화되었습니다.
- `hmd.enabled`가 `true`인 채로도 실제 HMD가 있으면 SteamVR은 그쪽을 사용합니다(`Using existing HMD`). VHMD의 컨트롤러는 좌우 역할 없이 남으므로 `controllers.enabled: false`를 권장합니다.
- 자세는 실제 HMD의 원시 좌표(`TrackingUniverseRawAndUncalibrated`)에 맞춰 보냅니다. 송신 프로그램에서 실제 HMD의 자세를 읽어 계산하거나, VMT 호환 OSC의 Joint / Follow로 실제 HMD(`HMD`)를 부모로 지정합니다. 트래커를 실제 HMD 아래 50 cm에 계속 두었을 때 머리를 움직여도 오차 중앙값은 0.7 mm였습니다.
- SteamVR의 "시작/종료 → 애드온 관리"에서 `vhmd`를 비활성화할 수도 있습니다.

### VHMD 와 실제 HMD 전환하기(Windows)

```powershell
python -m vhmd_client.mode pico    # 실제 HMD(PICO Connect) + VHMD 트래커
python -m vhmd_client.mode vhmd    # VHMD 의 HMD 와 컨트롤러(실제 HMD 드라이버는 끔)
python -m vhmd_client.mode         # 현재 모드 표시
```

FPS 조작 도구의 "Pico 로 전환" / "VHMD 로 전환" 버튼으로도 같은 일을 할 수 있습니다(전환 후 도구가 다시 열림).

- SteamVR 은 시작할 때 HMD 를 정하고 실행 중에는 바꿀 수 없으므로, 전환 시 SteamVR 을 다시 시작합니다(이 PC 에서 Pico 로 약 13 초, VHMD 로 약 19 초). 실행 중인 VR 앱은 종료됩니다.
- 바꾸는 것은 SteamVR 설정 2 개뿐입니다: `driver_pico.enable` 과 `driver_vhmd.configFile`. 후자는 설치된 설정에서 `hmd` / `controllers` 만 바꾼 사본(`%LOCALAPPDATA%\VHMD\profiles`)을 가리키며, 전환할 때마다 다시 만들므로 원래 설정의 변경이 그대로 반영됩니다.
- Pico 모드의 FPS 조작 도구에서는 머리가 실제 HMD 의 자세가 되고, 트래커가 착용자를 따라갑니다.

## 테스트

| 종류 | 명령 | 내용 |
|---|---|---|
| C++ 단위 테스트 | `ctest --preset vs2022-release` | 53건(파서, 좌표 변환, 설정, 타임아웃, 라우터, UDP, 라벨 생성, VMT 호환 OSC) |
| Python 단위 테스트 | `python -m pytest tests/python` | 14건(클라이언트, 프리뷰 읽기) |
| Linux 캡처 셀프 테스트 | `ctest`(Linux) | 실제 Vulkan으로 공유 이미지 → 공유 메모리를 확인(SteamVR 불필요) |
| Linux 실제 장비 확인 | `python3 tests/integration/linux_check.py` | 다이렉트 모드의 표시 경로 + 통합 테스트, 리포트 출력 |
| SteamVR 통합 테스트 | `python tests/integration/run_integration.py` | 58개 항목(열거, 온디맨드 추가, 오차, 지연, 입력, 스켈레톤, 핸드 모드 전환, 시선, 착용 센서, 배터리, OpenXR 핸드 트래킹, 잘못된 패킷, 부하, 종료 처리). `vmt.enabled: true`이면 VMT 호환 OSC 9개 항목이 추가됨 |
| 시작/종료 반복 | `python tests/integration/stress_restart.py --cycles 30` | 매번 드라이버의 종료 처리와 크래시 여부를 확인 |
| 장시간 안정성 | `python tests/integration/soak_test.py --minutes 30` | 프레임 레이트, 자세 오차, 메모리 증가, 경고를 일정 간격으로 기록 |

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
| 변경 이력 | [CHANGELOG](CHANGELOG.md) (일본어) | — | — | — |

검증 기록·판단 기록·변경 이력은 개발 기록이므로 일본어로만 제공됩니다.

## 저장소 구성

```
CMakeLists.txt / CMakePresets.json   빌드 설정(OpenVR SDK 가져오기, 플랫폼별 출력)
driver/
  driver.vrdrivermanifest
  resources/            설정, 입력 프로파일, 리매핑, 레거시 바인딩, 아이콘, 컨트롤러 모델
  src/
    main.cpp            HmdDriverFactory(유일한 공개 심볼)
    device_provider.*   디바이스 관리, 수신 / 갱신 스레드, 온디맨드 추가
    device_base.*       모든 디바이스 공통
    hmd_device.*        HMD, 가상 디스플레이, 시선
    linux/              Linux의 다이렉트 모드, 프레임 캡처, Vulkan 동적 로드
    windows/            Windows의 다이렉트 모드(D3D11), 프리뷰 창, 공유 메모리
    visuals/            SteamVR에 표시할 라벨(모델·아이콘) 생성
    controller_device.* 컨트롤러, 진동, 스켈레톤
    tracker_device.*    트래커, 롤
    math/ protocol/ config/ state/ net/ util/   OpenVR 비의존 코어(단위 테스트 대상)
clients/python/         vhmd_client 패키지와 examples/
tools/                  등록 / 해제 / 환경 확인(Windows: .ps1, Linux: .sh), 배포물 생성, 모델 생성
tests/                  unit/(C++), python/, integration/(SteamVR)
docs/                   문서(위의 표)
third_party/            OpenVR SDK(서브모듈 v2.15.6), nlohmann/json 3.11.3
```

## 동작 확인 환경

| 항목 | 버전 |
|---|---|
| SteamVR | 2.17.10(Windows 11, Linux: Ubuntu 26.04 + NVIDIA RTX 4000 Ada) |
| OpenVR SDK | v2.15.6 |
| Windows 컴파일러 | MSVC 14.44(Visual Studio 2022 Build Tools 17.14), CMake 4.3 |
| Linux 컴파일러 | gcc 13.3(Ubuntu 24.04 / WSL2), gcc 15.2(Ubuntu 26.04), CMake 3.28 / 4.2 |
| Python | 3.12(3.10 이상), 검증용으로 pyopenvr 2.12 |

결과 상세: [docs/verification.md](docs/verification.md) (일본어). Linux판 SteamVR에서는 v0.3.0의 다이렉트 모드로 실제 GPU(NVIDIA RTX 4000 Ada)에서 컴포지터의 90 fps 렌더링, 통합 테스트 28/28, Steam 클라이언트와 SteamVR Home이 있는 환경에서의 정상 종료, Proton을 통한 VRChat의 렌더링과 컨트롤러 추종까지 확인했습니다. GTX 1050에서는 SteamVR 컴포지터가 시작 후 잠시 뒤 GPU 예외로 멈춥니다([docs/ko/linux.md](docs/ko/linux.md)). VRChat에 로그인한 상태에서는 아바타 추종, 손가락, 이동, 메뉴 조작, 풀바디 보정을 Windows(컨트롤러)와 Linux(핸드 모드)에서 확인했습니다. 실제 HMD(Pico 4 Ultra + PICO Connect)에 트래커를 더하는 구성도 확인했습니다. 실제 화면이 있는 Linux 데스크톱, Linux에서의 프리뷰 시작·정지(경량화 이후)는 미검증입니다. 변경 이력: [CHANGELOG.md](CHANGELOG.md) (일본어)

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
