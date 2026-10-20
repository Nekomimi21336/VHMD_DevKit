**日本語** | [English](en/protocol.md) | [简体中文](zh-CN/protocol.md) | [한국어](ko/protocol.md) — [README に戻る](../README.md)

# 外部入力プロトコル (UDP / JSON, VMT互換OSC)

ドライバ `vhmd` は UDP で受信したメッセージを各仮想デバイスの「最新値バッファ」に書き込み、更新スレッド(既定 120 Hz)がそれを SteamVR に反映します。

| 項目 | 値 |
|---|---|
| トランスポート | UDP (IPv4) |
| 既定アドレス | `127.0.0.1:39570` (`vhmd_config.json` の `network.bind` / `network.port`) |
| 外部公開 | `network.allowRemote: true` の時だけ `127.0.0.1` 以外にバインド可(既定は不可) |
| フォーマット | 1 データグラム = JSON オブジェクト 1 個、または JSON 配列(バッチ、最大 256 件) |
| 最大サイズ | 65507 バイト(UDP 上限) |
| 文字コード | UTF-8 |
| VMT 互換 OSC | `vmt.enabled: true` の時、同じポートで受信(先頭バイトが `/` または `#` なら OSC と判定) |

## 座標系と単位

- **SteamVR 準拠**: 右手系、**+Y 上**、**-Z 前方**、+X 右、単位メートル。
- 回転は **クォータニオン `[w, x, y, z]`** で統一(正規化されていなくても受信時に正規化)。
- 速度 `vel` は m/s、角速度 `angvel` は rad/s(どちらもワールド座標系)。
- `pos` は「頭(HMD)の位置」です。首の位置として扱い SteamVR のネックモデルを適用したい場合は `hmd.headModel: true`。
- ポーズは `TrackingUniverseRawAndUncalibrated` にそのまま入ります(ルーム設定のオフセットは SteamVR 側で適用)。

## デバイス ID

| id | 種別 | SteamVR クラス / ロール | 既定シリアル |
|---|---|---|---|
| `hmd` | HMD | `TrackedDeviceClass_HMD` | `VHMD-HMD-0` |
| `left_hand` | コントローラ | `Controller` / `TrackedControllerRole_LeftHand` | `VHMD-CTL-L` |
| `right_hand` | コントローラ | `Controller` / `TrackedControllerRole_RightHand` | `VHMD-CTL-R` |
| `tracker_0` | トラッカー | `GenericTracker` / `TrackerRole_Waist` | `VHMD-TRK-0` |
| `tracker_1` | 〃 | `TrackerRole_LeftFoot` | `VHMD-TRK-1` |
| `tracker_2` | 〃 | `TrackerRole_RightFoot` | `VHMD-TRK-2` |
| `tracker_3` | 〃 | `TrackerRole_LeftKnee` | `VHMD-TRK-3` |
| `tracker_4` | 〃 | `TrackerRole_RightKnee` | `VHMD-TRK-4` |
| `tracker_5` | 〃 | `TrackerRole_LeftElbow` | `VHMD-TRK-5` |
| `tracker_6` | 〃 | `TrackerRole_RightElbow` | `VHMD-TRK-6` |
| `tracker_7` | 〃 | `TrackerRole_Chest` | `VHMD-TRK-7` |

- **基本は 3 点**(`hmd`, `left_hand`, `right_hand`)。この 3 台だけが SteamVR 起動時に登録されます。
- **トラッカーはオンデマンド**: そのトラッカー宛てのメッセージ(`pose` 等)が初めて届いた時点で SteamVR に追加され、届いたポーズから同期して始まります(実測で約 40 ms)。足 2 本を送れば 5 点、腰も送れば 6 点。`config` の `op: "add"` で先に追加することもできます。追加されたデバイスは SteamVR を終了するまで残り(OpenVR にデバイス削除 API が無いため)、送信が止まると `disconnect`(灰色表示)になります。起動時から全台登録したい場合は `"trackersOnDemand": false`。
- トラッカーは設定で最大 16 台。`id` とロールは `vhmd_config.json` の `trackers` 配列で変更できます。
- **ロール名でも指定可能**: 同じロールのトラッカーが 1 台だけなら `"id": "waist"` のように短いロール名で送れます(`tracker_0` と同じ)。
- ロール名は 3 形式を受け付けます: `waist` / `vive_tracker_waist` / `TrackerRole_Waist`。
  一覧: `handed, left_foot, right_foot, left_shoulder, right_shoulder, left_elbow, right_elbow, left_knee, right_knee, left_wrist, right_wrist, left_ankle, right_ankle, waist, chest, camera, keyboard`(SteamVR 2.17.10 同梱プロファイルに存在するもの)。

## メッセージ

### `pose`

```json
{ "type": "pose", "id": "hmd",
  "pos": [0.0, 1.7, 0.0], "rot": [1.0, 0.0, 0.0, 0.0],
  "vel": [0, 0, 0], "angvel": [0, 0, 0], "ts": 1760000000.123 }
```

| フィールド | 必須 | 説明 |
|---|---|---|
| `pos` | ○ | [x, y, z] m。各成分の絶対値 ≤ 1000 |
| `rot` | ○ | [w, x, y, z]。ノルム 0 / NaN は不正 |
| `vel`, `angvel` | | [x, y, z]。絶対値 ≤ 100。SteamVR の予測に使われます(無ければ 0) |
| `ts` | | 送信側 UNIX 時刻(秒)。診断用(`status` の `latency_ms`) |
| `valid` | | `false` で「トラッキング喪失」を明示。`pos`/`rot` は省略可。次の有効 `pose` で復帰 |

### `input`(コントローラ / HMD)

```json
{ "type": "input", "id": "right_hand", "name": "trigger", "value": 0.8 }
{ "type": "input", "id": "right_hand", "values": { "trigger": 0.8, "a": true, "joystick_x": -0.3 } }
```

値は数値(-1〜1)または真偽値。ボタンは 0.5 以上で押下。

| 名前 | SteamVR 入力パス | 型 / 範囲 |
|---|---|---|
| `trigger` | `/input/trigger/value` | 0〜1 |
| `trigger_click` / `trigger_touch` | `/input/trigger/click` / `touch` | bool |
| `grip` | `/input/grip/value` | 0〜1 |
| `grip_click` / `grip_touch` | `/input/grip/click` / `touch` | bool |
| `joystick_x`, `joystick_y` | `/input/joystick/x`, `y` | -1〜1(上が +y) |
| `joystick_click` / `joystick_touch` | `/input/joystick/click` / `touch` | bool |
| `a`, `b` (右手) / `x`, `y` (左手) | `/input/a/click` 等 | bool |
| `a_touch`, `b_touch`, `x_touch`, `y_touch` | `/input/a/touch` 等 | bool |
| `system`, `system_touch` | `/input/system/click` / `touch` | bool |
| `menu` | `/input/menu/click` | bool |

- 別名: `primary` = 右手 `a` / 左手 `x`、`secondary` = 右手 `b` / 左手 `y`。左手に `a`/`b` を送ると `x`/`y`、右手に `x`/`y` を送ると `a`/`b` として扱います。`thumbstick_*`, `stick_*` = `joystick_*`、`squeeze` = `grip`、`application_menu` = `menu`。
- 生パス(例 `"/input/y/click"`)もそのまま使えます。
- `trigger` / `grip` だけを送った場合、`*_click`(0.75 で ON / 0.65 で OFF)と `*_touch`(0.05 / 0.02)を自動生成します。`*_click` / `*_touch` を一度でも明示的に送るとその項目の自動生成は止まります。
- HMD は `system` のみ。
- 同じ更新周期内に届いた値は名前ごとに最新値へまとめられます。

### `skeleton`(左右コントローラの指)

```json
{ "type": "skeleton", "id": "left_hand", "curl": [0.0, 1.0, 1.0, 1.0, 1.0], "splay": [0, 0, 0, 0, 0] }
{ "type": "skeleton", "id": "left_hand", "bones": [[w,x,y,z], ... 31 個], "positions": [[x,y,z], ... 31 個] }
```

- `curl`: 親指, 人差し指, 中指, 薬指, 小指 の曲げ 0(開き)〜1(握り)。`splay`: -1〜1(省略時 0)。OpenVR サンプル `handskeletonsimulation` の手モデルでボーンを生成します。
- `bones`: OpenVR ハンドスケルトン(31 ボーン, `HandSkeletonBone` 順)の**親ボーン空間**の回転。`[w,x,y,z,px,py,pz]` で位置を同時指定するか、`positions` を別に渡せます(省略時は開いた手の参照位置)。
- 最後の `skeleton` から 500 ms 以上何も来ない間は、`trigger`/`grip`/ボタンのタッチ状態から指の曲げを自動生成します。
- SteamVR へは `WithController` / `WithoutController` 両方のモーションレンジに同じ値を送ります。
- コントローラーとハンドトラッキングを**SteamVR を止めずに切り替え**られます(`config` の `hand_mode`)。`controllers`(既定)は実機のコントローラーと同じ扱いで、骨格は Partial(SteamVR が指の骨を整えます)。`hands` では手ごとに別のデバイス(`VHMD-HAND-L/R`、骨格は Full で送ったボーンをそのまま反映)に入れ替わり、VRChat などはハンドトラッキングとして扱います(ボタンでのメニュー操作は無効になり、ジェスチャー操作になります)。どちらのモードでも ID は `left_hand` / `right_hand` のままで、送る内容を変える必要はありません。ハンド側のデバイスは初めて `hands` にしたときに追加されます。VRChat のメニューはどちらのモードでもレーザー + トリガーで操作できます。自動操作でレーザーを当てるときは、メニューの面に対して正面に近い角度で当ててください(浅い角度だと当たり判定に入りません)。

### `gaze`(HMD の視線)

```json
{ "type": "gaze", "id": "hmd", "origin": [0, 0, 0], "dir": [0, 0, -1] }
{ "type": "gaze", "id": "hmd", "origin": [0, 0, 0], "target": [0.1, 0.0, -1.0] }
{ "type": "gaze", "id": "hmd", "valid": false }
```

`IVRDriverInput::CreateEyeTrackingComponent("/eyetracking")` に `VREyeTrackingData_t`(origin, target)として渡します。

- **座標は HMD ローカル空間**(メートル、右手系、−Z が前)。`origin` は目の位置、`target` は注視点(方向ではなく点。`dir` を送ると `origin + dir` が注視点になります)。SteamVR が HMD の姿勢を掛けてアプリに渡します(実測で確認)。
- アプリ側での受け取り: OpenVR は `IVRInput::GetEyeTrackingDataRelativeToNow` / `GetEyeTrackingDataForNextFrame`(`eyetracking` 型のアクションを `/user/head/eyetracking` に割り当て)。OpenXR は `XR_EXT_eye_gaze_interaction`(`/user/eyes_ext/input/gaze_ext/pose`)。どちらも統合テストで確認しています。
- `valid: false` で、OpenVR は `bValid` / `bTracked` が false、OpenXR は位置の有効フラグが 0 になります。
- 両目まとめた 1 本の視線だけです(OpenVR の仕様。左右別やまぶたの開きはありません)。

### `config`(要求・応答)

```json
{ "type": "config", "op": "list", "seq": 1 }
```

`seq` は応答にそのまま返ります。応答は送信元アドレス/ポートへ UDP で返送されます。

| op | 引数 | 応答 |
|---|---|---|
| `list` (`status`) | | `{"type":"status", "devices":[...], "packets":{"total","rejected"}, ...}` |
| `ping` | | `{"type":"result","op":"ping","ok":true,"version":"0.1.0"}` |
| `roles` | | 利用可能なロール一覧 |
| `set_role` | `id`, `role` | SteamVR 設定の `trackers` セクションにロールを書き込み(ユーザー設定を上書き) |
| `reset` | `id`(省略時全台) | 設定ファイルの既定ポーズに戻す |
| `add` | `id`(省略時はオンデマンド全台) | オンデマンドのデバイスを今すぐ SteamVR に追加(次のフレームで反映)。`{"ok":true,"ids":[...]}` |
| `hand_mode` | `mode`(`controllers` / `hands`、省略時は現在のモードを返すだけ) | 左右の手をコントローラー / ハンドトラッキングで切り替え(SteamVR は止まりません)。`{"ok":true,"mode":"hands"}`。`status` にも `hand_mode` が入ります |
| `preview` | `mode`(`on` / `off`) | ダイレクトモード(Linux、または Windows で `display.mode = direct`)で、プレビュー(共有メモリへの映像書き出し)を開始 / 停止。既定では止まっていて、見る人がいないときは GPU のコピーも書き込みもしません(`preview_viewer.py` と `linux_check.py` は自動で開始・停止) |
| `refresh_rate` | `hz`(1〜240、省略時は現在値を返すだけ) | 仮想ディスプレイのフレームレートを変更(ダイレクトモード、SteamVR は止まりません)。`{"ok":true,"hz":30}` |
| `preview_window` | `mode`(`on` / `off`) | Windows のダイレクトモード(`display.mode = direct`)で、プレビューウィンドウ(HMD の映像)を表示 / 非表示。非表示のあいだは何もしません |
| (`status` の `perf`) | | ドライバ自身の負荷: `update_thread_cpu_pct`、`receive_thread_cpu_pct`(1 コア比の %、1 秒ごとに更新)、`update_tick_us_avg` / `update_tick_us_max`(更新 1 回の処理時間) |

`status` の `display`: `mode`(`window` / `direct`)、`width`、`height`、`refresh_hz`。ダイレクトモード(Linux)では加えて `swap_sets_created`、`swap_sets_alive`、`layers_submitted`、`presents`、`vsyncs`、`eye_texture`(`width`、`height`、`vk_format`)、`preview`(`state`、`submitted`、`written`、`active`)。

`status` の各デバイス: `id, serial, kind, role, enabled, on_demand, timeout_policy, index`(SteamVR デバイス番号、未登録は -1)`, pose_packets, input_packets, rate_hz`(受信レート)`, age_s`(最終受信からの秒数)`, latency_ms`(`ts` があれば)。

### ドライバ → クライアント: `haptic`

アプリが振動を要求すると、**最後にパケットを送ってきたアドレス**へ送ります。

```json
{ "type": "haptic", "id": "right_hand", "duration": 0.005, "frequency": 100.0, "amplitude": 0.75 }
```

## タイムアウト

最後の `pose` から `timeout.ms`(既定 500 ms)を過ぎたデバイスは、デバイス毎の `timeoutPolicy`、無ければ **オンデマンドのトラッカーは `disconnect`**、それ以外(HMD・コントローラ)は `timeout.policy`(既定 `hold`)に従います。起動直後の未受信状態も同様に扱います(起動時刻から計測)。

| policy | 挙動 |
|---|---|
| `hold`(既定) | 最後のポーズを有効なまま維持。速度は 0 にして予測で流れないようにする |
| `invalid` | `poseIsValid=false`, `TrackingResult_Running_OutOfRange` |
| `disconnect` | 上記 + `deviceIsConnected=false` |

## 不正パケット

以下は破棄され、`vrserver.txt` に警告(既定 5 件/秒にレート制限、抑制件数付き)が出ます。ドライバは停止しません。

- JSON として不正 / 型違い / 必須フィールド欠損 / 未知の `type`
- NaN・Inf、範囲外の値(位置 ±1000 m、速度 ±100、入力 ±1)、ノルム 0 のクォータニオン
- 未知・無効化されたデバイス id、ボーン数が 31 でない skeleton
- バッチ 256 件超、OSC で `vmt.enabled=false`、未対応 OSC アドレス

バッチ内の正常なメッセージは、他の要素が不正でも適用されます。

## VMT 互換 OSC(任意)

[Virtual Motion Tracker](https://gpsnmeajp.github.io/VirtualMotionTrackerDocument/) の送信側ツールをそのまま使うためのサブセットです。`vmt.enabled: true` で有効化。

| アドレス | 対応 |
|---|---|
| `/VMT/Room/Unity`, `/VMT/Raw/Unity` | ○ Unity 座標(左手系)→ Z 反転して変換 |
| `/VMT/Room/UEuler`, `/VMT/Raw/UEuler` | ○ Unity オイラー角(度) |
| `/VMT/Room/Driver`, `/VMT/Raw/Driver` | ○ ドライバ座標そのまま |
| `/VMT/Input/Button[/Touch]` | ○ buttonIndex 0=system,1=a(左手 x),2=b(左手 y),3=menu,4=joystick_click,5=trigger_click,6=grip_click |
| `/VMT/Input/Trigger[/Touch|/Click]` | ○ triggerIndex 0=trigger, 1=grip |
| `/VMT/Input/Joystick[/Touch|/Click]` | ○ joystickIndex 0 のみ |
| `/VMT/Joint/*`, `/VMT/Follow/*`, `/VMT/Skeleton/*`, `/VMT/SetRoomMatrix` 等 | × 未対応(破棄してログ) |

- `index` → デバイス: `vmt.indexMap`(例 `{"0": "tracker_0", "1": "left_hand"}`)、無指定なら `tracker_<index>`。
- `enable` 2/5 → `left_hand`、3/6 → `right_hand`、0 → そのデバイスのポーズ無効化。
- Room と Raw は区別しません(ルーム行列は未実装)。
- VMT 本体と同じポート 39570 を使うため、VMT ドライバと同時に有効化しないでください。

## クライアント実装の例

Python: `clients/python/vhmd_client`(依存なし)。

```python
from vhmd_client import VHMDClient, math3d
with VHMDClient() as c:
    with c.batch():                      # 1 データグラムにまとめる
        c.pose("hmd", (0, 1.7, 0), math3d.from_euler(yaw=30))
        c.pose("waist", (0, 1.0, 0))
    c.inputs("right_hand", {"trigger": 1.0, "a": True})
    print(c.status()["devices"][0])
```

他言語は「JSON を UDP で `127.0.0.1:39570` に送る」だけで実装できます。
