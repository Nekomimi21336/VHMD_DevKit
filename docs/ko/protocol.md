[日本語](../protocol.md) | [English](../en/protocol.md) | [简体中文](../zh-CN/protocol.md) | **한국어** — [README로 돌아가기](../../README.ko.md)

# 외부 입력 프로토콜 (UDP / JSON, VMT 호환 OSC)

드라이버 `vhmd`는 UDP로 수신한 메시지를 각 가상 디바이스의 "최신 값 버퍼"에 기록하고, 갱신 스레드(기본 120 Hz)가 이를 SteamVR에 반영합니다.

| 항목 | 값 |
|---|---|
| 전송 계층 | UDP (IPv4) |
| 기본 주소 | `127.0.0.1:39570` (`vhmd_config.json`의 `network.bind` / `network.port`) |
| 외부 공개 | `network.allowRemote: true`일 때만 `127.0.0.1` 이외에 바인드 가능(기본값은 불가) |
| 포맷 | 데이터그램 1개 = JSON 객체 1개, 또는 JSON 배열(배치, 최대 256건) |
| 최대 크기 | 65507바이트(UDP 상한) |
| 문자 인코딩 | UTF-8 |
| VMT 호환 OSC | `vmt.enabled: true`일 때 같은 포트로 수신(첫 바이트가 `/` 또는 `#`이면 OSC로 판정) |

## 좌표계와 단위

- **SteamVR 기준**: 오른손 좌표계, **+Y 위**, **-Z 전방**, +X 오른쪽, 단위는 미터.
- 회전은 **쿼터니언 `[w, x, y, z]`**로 통일(정규화되어 있지 않아도 수신 시 정규화).
- 속도 `vel`은 m/s, 각속도 `angvel`은 rad/s(둘 다 월드 좌표계).
- `pos`는 "머리(HMD)의 위치"입니다. 목의 위치로 취급하여 SteamVR의 넥 모델을 적용하려면 `hmd.headModel: true`.
- 포즈는 `TrackingUniverseRawAndUncalibrated`에 그대로 들어갑니다(룸 설정의 오프셋은 SteamVR 쪽에서 적용).

## 디바이스 ID

| id | 종류 | SteamVR 클래스 / 롤 | 기본 시리얼 |
|---|---|---|---|
| `hmd` | HMD | `TrackedDeviceClass_HMD` | `VHMD-HMD-0` |
| `left_hand` | 컨트롤러 | `Controller` / `TrackedControllerRole_LeftHand` | `VHMD-CTL-L` |
| `right_hand` | 컨트롤러 | `Controller` / `TrackedControllerRole_RightHand` | `VHMD-CTL-R` |
| `tracker_0` | 트래커 | `GenericTracker` / `TrackerRole_Waist` | `VHMD-TRK-0` |
| `tracker_1` | 〃 | `TrackerRole_LeftFoot` | `VHMD-TRK-1` |
| `tracker_2` | 〃 | `TrackerRole_RightFoot` | `VHMD-TRK-2` |
| `tracker_3` | 〃 | `TrackerRole_LeftKnee` | `VHMD-TRK-3` |
| `tracker_4` | 〃 | `TrackerRole_RightKnee` | `VHMD-TRK-4` |
| `tracker_5` | 〃 | `TrackerRole_LeftElbow` | `VHMD-TRK-5` |
| `tracker_6` | 〃 | `TrackerRole_RightElbow` | `VHMD-TRK-6` |
| `tracker_7` | 〃 | `TrackerRole_Chest` | `VHMD-TRK-7` |

- **기본은 3점**(`hmd`, `left_hand`, `right_hand`). 이 3대만 SteamVR 시작 시에 등록됩니다.
- **트래커는 온디맨드**: 해당 트래커 앞으로 보낸 메시지(`pose` 등)가 처음 도착한 시점에 SteamVR에 추가되며, 도착한 포즈에서 동기화된 상태로 시작합니다(실측 약 40 ms). 발 2개를 보내면 5점, 허리도 보내면 6점. `config`의 `op: "add"`로 미리 추가할 수도 있습니다. 추가된 디바이스는 SteamVR을 종료할 때까지 남아 있으며(OpenVR에 디바이스 삭제 API가 없기 때문), 송신이 멈추면 `disconnect`(회색 표시)가 됩니다. 시작 시부터 모든 대수를 등록하려면 `"trackersOnDemand": false`.
- 트래커는 설정으로 최대 16대. `id`와 롤은 `vhmd_config.json`의 `trackers` 배열에서 변경할 수 있습니다.
- **롤 이름으로도 지정 가능**: 같은 롤의 트래커가 1대뿐이라면 `"id": "waist"`처럼 짧은 롤 이름으로 보낼 수 있습니다(`tracker_0`과 동일).
- 롤 이름은 3가지 형식을 받습니다: `waist` / `vive_tracker_waist` / `TrackerRole_Waist`.
  목록: `handed, left_foot, right_foot, left_shoulder, right_shoulder, left_elbow, right_elbow, left_knee, right_knee, left_wrist, right_wrist, left_ankle, right_ankle, waist, chest, camera, keyboard`(SteamVR 2.17.10 동봉 프로파일에 존재하는 것).

## 메시지

### `pose`

```json
{ "type": "pose", "id": "hmd",
  "pos": [0.0, 1.7, 0.0], "rot": [1.0, 0.0, 0.0, 0.0],
  "vel": [0, 0, 0], "angvel": [0, 0, 0], "ts": 1760000000.123 }
```

| 필드 | 필수 | 설명 |
|---|---|---|
| `pos` | ○ | [x, y, z] m. 각 성분의 절댓값 ≤ 1000 |
| `rot` | ○ | [w, x, y, z]. 노름 0 / NaN은 잘못된 값 |
| `vel`, `angvel` | | [x, y, z]. 절댓값 ≤ 100. SteamVR의 예측에 사용됩니다(없으면 0) |
| `ts` | | 송신 측 UNIX 시각(초). 진단용(`status`의 `latency_ms`) |
| `valid` | | `false`로 "트래킹 상실"을 명시. `pos`/`rot`는 생략 가능. 다음 유효한 `pose`로 복귀 |

### `input`(컨트롤러 / HMD)

```json
{ "type": "input", "id": "right_hand", "name": "trigger", "value": 0.8 }
{ "type": "input", "id": "right_hand", "values": { "trigger": 0.8, "a": true, "joystick_x": -0.3 } }
```

값은 숫자(-1~1) 또는 불리언. 버튼은 0.5 이상에서 눌림.

| 이름 | SteamVR 입력 경로 | 타입 / 범위 |
|---|---|---|
| `trigger` | `/input/trigger/value` | 0~1 |
| `trigger_click` / `trigger_touch` | `/input/trigger/click` / `touch` | bool |
| `grip` | `/input/grip/value` | 0~1 |
| `grip_click` / `grip_touch` | `/input/grip/click` / `touch` | bool |
| `joystick_x`, `joystick_y` | `/input/joystick/x`, `y` | -1~1(위가 +y) |
| `joystick_click` / `joystick_touch` | `/input/joystick/click` / `touch` | bool |
| `a`, `b` (오른손) / `x`, `y` (왼손) | `/input/a/click` 등 | bool |
| `a_touch`, `b_touch`, `x_touch`, `y_touch` | `/input/a/touch` 등 | bool |
| `system`, `system_touch` | `/input/system/click` / `touch` | bool |
| `menu` | `/input/menu/click` | bool |

- 별칭: `primary` = 오른손 `a` / 왼손 `x`, `secondary` = 오른손 `b` / 왼손 `y`. 왼손에 `a`/`b`를 보내면 `x`/`y`로, 오른손에 `x`/`y`를 보내면 `a`/`b`로 취급합니다. `thumbstick_*`, `stick_*` = `joystick_*`, `squeeze` = `grip`, `application_menu` = `menu`.
- 원시 경로(예: `"/input/y/click"`)도 그대로 사용할 수 있습니다.
- `trigger` / `grip`만 보낸 경우, `*_click`(0.75에서 ON / 0.65에서 OFF)과 `*_touch`(0.05 / 0.02)를 자동 생성합니다. `*_click` / `*_touch`를 한 번이라도 명시적으로 보내면 해당 항목의 자동 생성은 중지됩니다.
- HMD는 `system`만.
- 같은 갱신 주기 내에 도착한 값은 이름별로 최신 값으로 합쳐집니다.

### `skeleton`(좌우 컨트롤러의 손가락)

```json
{ "type": "skeleton", "id": "left_hand", "curl": [0.0, 1.0, 1.0, 1.0, 1.0], "splay": [0, 0, 0, 0, 0] }
{ "type": "skeleton", "id": "left_hand", "bones": [[w,x,y,z], ... 31개], "positions": [[x,y,z], ... 31개] }
```

- `curl`: 엄지, 검지, 중지, 약지, 소지의 굽힘 0(펼침)~1(쥠). `splay`: -1~1(생략 시 0). OpenVR 샘플 `handskeletonsimulation`의 손 모델로 본을 생성합니다.
- `bones`: OpenVR 핸드 스켈레톤(31개 본, `HandSkeletonBone` 순서)의 **부모 본 공간** 회전. `[w,x,y,z,px,py,pz]`로 위치를 함께 지정하거나, `positions`를 별도로 전달할 수 있습니다(생략 시 펼친 손의 참조 위치).
- 마지막 `skeleton` 이후 500 ms 이상 아무것도 오지 않는 동안에는 `trigger`/`grip`/버튼의 터치 상태로부터 손가락 굽힘을 자동 생성합니다.
- SteamVR에는 `WithController` / `WithoutController` 두 모션 레인지 모두에 같은 값을 보냅니다.
- 컨트롤러와 핸드 트래킹을 **SteamVR을 멈추지 않고 전환**할 수 있습니다(`config`의 `hand_mode`). `controllers`(기본)는 실제 컨트롤러와 같은 취급이며 스켈레톤은 Partial(SteamVR이 손가락 본을 정리)입니다. `hands`에서는 손마다 별도의 디바이스(`VHMD-HAND-L/R`, 스켈레톤은 Full로 보낸 본을 그대로 반영)로 교체되며, VRChat 등은 핸드 트래킹으로 취급합니다(버튼으로 메뉴 조작은 비활성화되고 제스처 조작이 됩니다). 두 모드 모두 ID는 `left_hand` / `right_hand` 그대로이므로 보내는 내용을 바꿀 필요가 없습니다. 핸드 디바이스는 처음 `hands`로 전환할 때 추가됩니다.

### `gaze`(HMD의 시선)

```json
{ "type": "gaze", "id": "hmd", "origin": [0, 0, 0], "dir": [0, 0, -1] }
{ "type": "gaze", "id": "hmd", "origin": [0, 0, 0], "target": [0.1, 0.0, -1.0] }
{ "type": "gaze", "id": "hmd", "valid": false }
```

`IVRDriverInput::CreateEyeTrackingComponent("/eyetracking")`에 `VREyeTrackingData_t`(origin, target)로 전달합니다. 좌표 공간은 SteamVR 헤더에 명시되어 있지 않아 **HMD 로컬 공간을 가정(미검증)**. `docs/decisions.md`(일본어) 참조.

### `config`(요청·응답)

```json
{ "type": "config", "op": "list", "seq": 1 }
```

`seq`는 응답에 그대로 반환됩니다. 응답은 송신 측 주소/포트로 UDP를 통해 반송됩니다.

| op | 인수 | 응답 |
|---|---|---|
| `list` (`status`) | | `{"type":"status", "devices":[...], "packets":{"total","rejected"}, ...}` |
| `ping` | | `{"type":"result","op":"ping","ok":true,"version":"0.1.0"}` |
| `roles` | | 사용 가능한 롤 목록 |
| `set_role` | `id`, `role` | SteamVR 설정의 `trackers` 섹션에 롤을 기록(사용자 설정을 덮어씀) |
| `reset` | `id`(생략 시 전체) | 설정 파일의 기본 포즈로 되돌림 |
| `add` | `id`(생략 시 온디맨드 전체) | 온디맨드 디바이스를 즉시 SteamVR에 추가(다음 프레임에 반영). `{"ok":true,"ids":[...]}` |
| `hand_mode` | `mode`(`controllers` / `hands`, 생략 시 현재 모드만 반환) | 좌우 손을 컨트롤러 / 핸드 트래킹으로 전환(SteamVR은 멈추지 않음). `{"ok":true,"mode":"hands"}`. `status`에도 `hand_mode`가 포함됨 |

`status`의 `display`: `mode`(`window` / `direct`), `width`, `height`, `refresh_hz`. 다이렉트 모드(Linux)에서는 추가로 `swap_sets_created`, `swap_sets_alive`, `layers_submitted`, `presents`, `vsyncs`, `eye_texture`(`width`, `height`, `vk_format`), `preview`(`state`, `submitted`, `written`, `active`).

`status`의 각 디바이스: `id, serial, kind, role, enabled, on_demand, timeout_policy, index`(SteamVR 디바이스 번호, 미등록은 -1)`, pose_packets, input_packets, rate_hz`(수신 레이트)`, age_s`(마지막 수신 이후 초)`, latency_ms`(`ts`가 있는 경우).

### 드라이버 → 클라이언트: `haptic`

앱이 진동을 요청하면 **마지막으로 패킷을 보낸 주소**로 보냅니다.

```json
{ "type": "haptic", "id": "right_hand", "duration": 0.005, "frequency": 100.0, "amplitude": 0.75 }
```

## 타임아웃

마지막 `pose`로부터 `timeout.ms`(기본 500 ms)가 지난 디바이스는 디바이스별 `timeoutPolicy`를 따르며, 없으면 **온디맨드 트래커는 `disconnect`**, 그 외(HMD·컨트롤러)는 `timeout.policy`(기본 `hold`)를 따릅니다. 시작 직후의 미수신 상태도 동일하게 취급합니다(시작 시각부터 측정).

| policy | 동작 |
|---|---|
| `hold`(기본) | 마지막 포즈를 유효한 상태로 유지. 속도는 0으로 하여 예측으로 흘러가지 않도록 함 |
| `invalid` | `poseIsValid=false`, `TrackingResult_Running_OutOfRange` |
| `disconnect` | 위 내용 + `deviceIsConnected=false` |

## 잘못된 패킷

다음은 폐기되며, `vrserver.txt`에 경고(기본 초당 5건으로 레이트 제한, 억제 건수 포함)가 출력됩니다. 드라이버는 정지하지 않습니다.

- JSON으로서 잘못됨 / 타입 불일치 / 필수 필드 누락 / 알 수 없는 `type`
- NaN·Inf, 범위 밖의 값(위치 ±1000 m, 속도 ±100, 입력 ±1), 노름 0인 쿼터니언
- 알 수 없거나 비활성화된 디바이스 id, 본 수가 31이 아닌 skeleton
- 배치 256건 초과, `vmt.enabled=false`일 때의 OSC, 지원하지 않는 OSC 주소

배치 내의 정상 메시지는 다른 요소가 잘못되어 있어도 적용됩니다.

## VMT 호환 OSC(선택)

[Virtual Motion Tracker](https://gpsnmeajp.github.io/VirtualMotionTrackerDocument/)의 송신 측 도구를 그대로 사용하기 위한 서브셋입니다. `vmt.enabled: true`로 활성화.

| 주소 | 지원 |
|---|---|
| `/VMT/Room/Unity`, `/VMT/Raw/Unity` | ○ Unity 좌표(왼손 좌표계) → Z 반전하여 변환 |
| `/VMT/Room/UEuler`, `/VMT/Raw/UEuler` | ○ Unity 오일러 각(도) |
| `/VMT/Room/Driver`, `/VMT/Raw/Driver` | ○ 드라이버 좌표 그대로 |
| `/VMT/Input/Button[/Touch]` | ○ buttonIndex 0=system,1=a(왼손 x),2=b(왼손 y),3=menu,4=joystick_click,5=trigger_click,6=grip_click |
| `/VMT/Input/Trigger[/Touch|/Click]` | ○ triggerIndex 0=trigger, 1=grip |
| `/VMT/Input/Joystick[/Touch|/Click]` | ○ joystickIndex 0만 |
| `/VMT/Joint/*`, `/VMT/Follow/*`, `/VMT/Skeleton/*`, `/VMT/SetRoomMatrix` 등 | × 미지원(폐기하고 로그 출력) |

- `index` → 디바이스: `vmt.indexMap`(예: `{"0": "tracker_0", "1": "left_hand"}`), 지정하지 않으면 `tracker_<index>`.
- `enable` 2/5 → `left_hand`, 3/6 → `right_hand`, 0 → 해당 디바이스의 포즈 무효화.
- Room과 Raw는 구별하지 않습니다(룸 행렬은 미구현).
- VMT 본체와 같은 포트 39570을 사용하므로 VMT 드라이버와 동시에 활성화하지 마십시오.

## 클라이언트 구현 예

Python: `clients/python/vhmd_client`(의존성 없음).

```python
from vhmd_client import VHMDClient, math3d
with VHMDClient() as c:
    with c.batch():                      # 데이터그램 1개로 묶음
        c.pose("hmd", (0, 1.7, 0), math3d.from_euler(yaw=30))
        c.pose("waist", (0, 1.0, 0))
    c.inputs("right_hand", {"trigger": 1.0, "a": True})
    print(c.status()["devices"][0])
```

다른 언어는 "JSON을 UDP로 `127.0.0.1:39570`에 보내기"만으로 구현할 수 있습니다.
