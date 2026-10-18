[日本語](../architecture.md) | [English](../en/architecture.md) | [简体中文](../zh-CN/architecture.md) | **한국어** — [README로 돌아가기](../../README.ko.md)

# 아키텍처

## 전체 구조

```
[외부 클라이언트 (Python / 임의 언어 / VMT 송신 도구)]
        │ UDP 127.0.0.1:39570  (JSON or VMT-OSC)
        ▼
┌───────────────────────── driver_vhmd.dll (vrserver 프로세스 내) ─────────────────────────┐
│ UdpReceiver 스레드 ──► MessageRouter ──► DeviceState (디바이스별, mutex 보호)              │
│   net/udp_*              state/router       state/device_state                            │
│   (OS 의존은 여기뿐)      protocol/parser    ·최신 포즈 / 입력 / 스켈레톤 / 시선           │
│                          protocol/osc       ·타임아웃 판정·스무딩                        │
│                                                    │                                     │
│ 갱신 스레드 (기본 120Hz, 고분해능 타이머) ──────────┘                                     │
│   DeviceProvider::UpdateLoop → VirtualDevice::Update                                      │
│     → TrackedDevicePoseUpdated / UpdateBoolean/Scalar/Skeleton/EyeTrackingComponent       │
│                                                                                           │
│ RunFrame (vrserver 메인 스레드) → PollNextEvent → 진동 이벤트를 UDP로 클라이언트에 전송    │
└───────────────────────────────────────────────────────────────────────────────────────────┘
        ▼
     SteamVR (vrserver) → 앱 (VRChat 등)
```

## 레이어

| 레이어 | 디렉터리 | OpenVR 의존 | 단위 테스트 |
|---|---|---|---|
| 수학 | `driver/src/math/` | 없음 | ○ |
| 프로토콜(JSON / OSC) | `driver/src/protocol/` | 없음 | ○ |
| 설정·롤 | `driver/src/config/` | 없음 | ○ |
| 상태·라우팅 | `driver/src/state/` | 없음 | ○ |
| 네트워크 | `driver/src/net/` | 없음(WinSock/BSD를 내부에서 전환) | ○(루프백) |
| 유틸리티 | `driver/src/util/` | 없음 | 일부 |
| 디바이스·프로바이더 | `driver/src/*.cpp` | 있음 | 통합 테스트 |

OpenVR 비의존 부분은 정적 라이브러리 `vhmd_core`로 묶여 있으며, 드라이버 DLL과 테스트 실행 파일 양쪽이 링크합니다.

## 스레드와 라이프사이클

| 스레드 | 역할 | 종료 |
|---|---|---|
| vrserver 메인 | `Init` / `RunFrame` / `Cleanup`, `Activate` / `Deactivate` | — |
| 수신 | `UdpReceiver::Loop`. 100 ms마다 poll이 타임아웃되어 정지 플래그를 확인 | `Cleanup`에서 `Stop()` → join(최대 ~100 ms) |
| 갱신 | `DeviceProvider::UpdateLoop`. 고정 주기로 모든 디바이스를 갱신 | `Cleanup`에서 정지 플래그 → join(최대 1주기) |

- `Init`: 설정 로드 → 모든 디바이스의 `DeviceState`와 디바이스 객체를 생성 → **온디맨드가 아닌** 디바이스(기본적으로 HMD와 좌우 컨트롤러의 3점)만 `TrackedDeviceAdded` → 수신·갱신 스레드 시작.
- 온디맨드 추가: 수신 스레드가 미등록 트래커 앞으로 온 메시지를 받으면, 값을 버퍼에 넣은 뒤 `RequestAdd(id)`(큐에 쌓기만 함). `RunFrame`(vrserver 메인 스레드)이 `ProcessPendingAdds()`로 큐를 처리하여 `TrackedDeviceAdded`를 호출. 버퍼된 값이 `Activate` 후 100 ms부터 전송되므로, 추가 직후부터 동기화되어 있음.
- `Activate(index)`: 프로퍼티·입력 컴포넌트 생성. 디바이스별 mutex로 갱신 스레드와 상호 배제.
- 갱신 스레드는 `Activate` 후 100 ms가 지날 때까지 해당 디바이스의 포즈를 보내지 않습니다(vrserver가 디바이스 소유권 등록을 마치기 전에 보내면 `PoseUpdated ... owned by driver (-1)` 오류가 발생하기 때문. 실측으로 확인).
- `Deactivate`: 인덱스를 무효화 → 이후 갱신 스레드는 해당 디바이스를 건드리지 않습니다.
- `Cleanup`: 수신 정지 → 갱신 정지 → 디바이스 파기 → 로그 싱크 해제. SteamVR 종료 시 행(hang)이 걸리지 않음을 31회 연속 시작/종료로 확인(`docs/verification.md`, 일본어).
- 수신 핸들러 내의 예외는 모두 포착되며, 수신 스레드는 계속 동작합니다.

## 데이터 흐름상의 규칙

- 수신 측은 "최신 값으로 덮어쓰기". 큐를 갖지 않으므로 고빈도 송신(1000 pps 이상)에서도 지연이 누적되지 않습니다.
- 입력 값은 디바이스별·이름별로 최신 값으로 합쳐 갱신 스레드에서 SteamVR에 반영.
- 지연 내역: UDP 수신은 즉시, SteamVR로의 반영은 다음 갱신 주기(120 Hz라면 최대 8.3 ms). 실측한 송신→앱 가시까지의 시간은 중앙값 8.2 ms(`docs/verification.md`, 일본어).
- 포즈는 `TrackedDevicePoseUpdated`로 매 주기 보냅니다(SteamVR은 갱신이 멈춘 디바이스를 오래된 것으로 간주하기 때문).

## 확장 포인트

| 하고 싶은 것 | 수정할 곳 |
|---|---|
| 새로운 디바이스 종류 | `VirtualDevice`를 상속(`OnActivate`에서 프로퍼티/컴포넌트, `OnInput`/`OnTick` 등을 오버라이드)하고 `DeviceKind`와 `DeviceProvider::CreateDevices`에 추가 |
| 새로운 메시지 타입 | `protocol/messages.h`에 구조체 추가 → `parser.cpp`의 `ParseOne` → `MessageRouter::HandleDatagram` → `DeviceState` → 디바이스의 훅 |
| 새로운 전송 계층 (WebSocket, 공유 메모리 등) | `UdpReceiver`와 같은 형태로 수신하여 `MessageRouter::HandleDatagram()`를 호출하기만 하면 됨(라우터는 전송 계층 비의존) |
| 다른 프로토콜 호환 | `protocol/osc.*`와 마찬가지로 `ParseResult`를 반환하는 파서를 추가하고 `HandleDatagram`의 판정에 추가 |
| 실제 화면 표시(향후) | `HmdDevice::GetComponent`에서 `IVRDisplayComponent` 대신 `IVRDriverDirectModeComponent` / `IVRVirtualDisplay`를 반환하는 구현으로 교체 |
| 컨트롤러 입력 추가 | `controller_device.cpp`의 `InputDef` 표, `resources/input/vhmd_controller_profile.json`, 필요하면 리매핑(`tools/gen_remapping.py`)과 레거시 바인딩 |

## 플랫폼 추상화(향후 Linux 지원)

- `util/platform.h`: `VHMD_EXPORT`(`__declspec(dllexport)` / `visibility("default")`), `kPathSeparator`.
- `net/udp_socket.cpp`: WinSock과 BSD 소켓을 `#if`로 전환. 헤더는 OS 비의존(핸들은 `intptr_t`).
- `util/precise_sleep.cpp`: Windows는 고분해능 Waitable Timer(`timeBeginPeriod`에 의한 시스템 전체 타이머 변경은 하지 않음), 그 외는 `sleep_until`.
- CMake: `CMAKE_CXX_VISIBILITY_PRESET hidden`, 출력 위치 `bin/<win64|linux64|osx64>/driver_vhmd.<dll|so>`, `PREFIX ""`.
- 설정 파일 경로는 `std::filesystem::u8path`로 UTF-8로 취급.
- Linux: WSL2에서 빌드·단위 테스트 완료. ELF에서는 `driver_exports.map`으로 `HmdDriverFactory` 이외를 비공개로 하고, libstdc++/libgcc를 정적 링크. Linux판 SteamVR에서의 동작은 미검증(`docs/linux.md`).

## SteamVR에 등록하는 이름·ID

| 종류 | 값 |
|---|---|
| 드라이버 이름 | `vhmd`(`driver.vrdrivermanifest`) |
| 컨트롤러 타입 | `vhmd_hmd`, `vhmd_controller`, `vhmd_tracker`(독자 타입. 타사 이름은 사용하지 않음) |
| 제조사 이름 | `VirtualHMD (development)` |
| 트래킹 시스템 이름 | `vhmd` |
| 렌더 모델 | SteamVR 동봉 범용 모델 `generic_hmd` / `generic_controller` / `generic_tracker` |
| 등록 디바이스 타입 | `vhmd/<serial>`(트래커 롤의 설정 키 `/devices/vhmd/<serial>`에 대응) |
