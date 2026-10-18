**日本語** | [English](en/architecture.md) | [简体中文](zh-CN/architecture.md) | [한국어](ko/architecture.md) — [README に戻る](../README.md)

# アーキテクチャ

## 全体像

```
[外部クライアント (Python / 任意言語 / VMT送信ツール)]
        │ UDP 127.0.0.1:39570  (JSON or VMT-OSC)
        ▼
┌───────────────────────── driver_vhmd.dll (vrserver プロセス内) ─────────────────────────┐
│ UdpReceiver スレッド ──► MessageRouter ──► DeviceState (デバイス毎, mutex 保護)            │
│   net/udp_*              state/router       state/device_state                            │
│   (OS依存はここだけ)      protocol/parser    ・最新ポーズ / 入力 / スケルトン / 視線        │
│                          protocol/osc       ・タイムアウト判定・スムージング               │
│                                                    │                                     │
│ 更新スレッド (既定120Hz, 高分解能タイマ) ───────────┘                                     │
│   DeviceProvider::UpdateLoop → VirtualDevice::Update                                      │
│     → TrackedDevicePoseUpdated / UpdateBoolean/Scalar/Skeleton/EyeTrackingComponent       │
│                                                                                           │
│ RunFrame (vrserver メインスレッド) → PollNextEvent → 振動イベントを UDP でクライアントへ   │
└───────────────────────────────────────────────────────────────────────────────────────────┘
        ▼
     SteamVR (vrserver) → アプリ (VRChat 等)
```

## レイヤ

| レイヤ | ディレクトリ | OpenVR 依存 | 単体テスト |
|---|---|---|---|
| 数学 | `driver/src/math/` | なし | ○ |
| プロトコル(JSON / OSC) | `driver/src/protocol/` | なし | ○ |
| 設定・ロール | `driver/src/config/` | なし | ○ |
| 状態・ルーティング | `driver/src/state/` | なし | ○ |
| ネットワーク | `driver/src/net/` | なし(WinSock/BSD を内部で切替) | ○(ループバック) |
| ユーティリティ | `driver/src/util/` | なし | 一部 |
| デバイス・プロバイダ | `driver/src/*.cpp` | あり | 統合テスト |

OpenVR 非依存部分は静的ライブラリ `vhmd_core` にまとまっており、ドライバ DLL とテスト実行ファイルの両方がリンクします。

## スレッドとライフサイクル

| スレッド | 役割 | 終了 |
|---|---|---|
| vrserver メイン | `Init` / `RunFrame` / `Cleanup`、`Activate` / `Deactivate` | — |
| 受信 | `UdpReceiver::Loop`。100 ms 毎に poll がタイムアウトし停止フラグを確認 | `Cleanup` で `Stop()` → join(最大 ~100 ms) |
| 更新 | `DeviceProvider::UpdateLoop`。固定周期で全デバイスを更新 | `Cleanup` で停止フラグ → join(最大 1 周期) |

- `Init`: 設定読込 → 全デバイスの `DeviceState` とデバイスオブジェクトを生成 → **オンデマンドでない**デバイス(既定では HMD と左右コントローラの 3 点)だけ `TrackedDeviceAdded` → 受信・更新スレッド開始。
- オンデマンド追加: 受信スレッドが未登録トラッカー宛てのメッセージを受けると、値をバッファに入れてから `RequestAdd(id)`(キューに積むだけ)。`RunFrame`(vrserver メインスレッド)が `ProcessPendingAdds()` でキューを処理し `TrackedDeviceAdded` を呼ぶ。バッファ済みの値が `Activate` 後 100 ms から送られるので、追加直後から同期している。
- `Activate(index)`: プロパティ・入力コンポーネント作成。デバイス毎の mutex で更新スレッドと排他。
- 更新スレッドは `Activate` 後 100 ms 経つまでそのデバイスのポーズを送りません(vrserver がデバイス所有権を登録し終える前に送ると `PoseUpdated ... owned by driver (-1)` エラーになるため。実測で確認)。
- `Deactivate`: インデックスを無効化 → 以後、更新スレッドはそのデバイスに触れません。
- `Cleanup`: 受信停止 → 更新停止 → デバイス破棄 → ログシンク解除。SteamVR 終了時にハングしないことを 31 回連続の起動終了で確認(`docs/verification.md`)。
- 受信ハンドラ内の例外はすべて捕捉され、受信スレッドは継続します。

## データフロー上の決まり

- 受信側は「最新値で上書き」。キューは持たず、高頻度送信(1000 pps 以上)でも遅延が積み上がりません。
- 入力値はデバイス毎・名前毎に最新値へまとめ、更新スレッドで SteamVR へ反映。
- 遅延の内訳: UDP 受信は即時、SteamVR への反映は次の更新周期(120 Hz なら最大 8.3 ms)。実測の送信→アプリ可視は中央値 8.2 ms(`docs/verification.md`)。
- ポーズは `TrackedDevicePoseUpdated` に毎周期送ります(SteamVR は更新が止まったデバイスを古いとみなすため)。

## 拡張ポイント

| やりたいこと | 触る場所 |
|---|---|
| 新しいデバイス種別 | `VirtualDevice` を継承(`OnActivate` で プロパティ/コンポーネント、`OnInput`/`OnTick` 等をオーバーライド)し、`DeviceKind` と `DeviceProvider::CreateDevices` に追加 |
| 新しいメッセージ型 | `protocol/messages.h` に構造体を追加 → `parser.cpp` の `ParseOne` → `MessageRouter::HandleDatagram` → `DeviceState` → デバイスのフック |
| 新しいトランスポート (WebSocket, 共有メモリ等) | `UdpReceiver` と同じ形で受信し `MessageRouter::HandleDatagram()` を呼ぶだけ(ルーターはトランスポート非依存) |
| 別プロトコル互換 | `protocol/osc.*` と同様に `ParseResult` を返すパーサを追加し、`HandleDatagram` の判定に追加 |
| 実画面表示(将来) | `HmdDevice::GetComponent` で `IVRDisplayComponent` の代わりに `IVRDriverDirectModeComponent` / `IVRVirtualDisplay` を返す実装に差し替え |
| コントローラ入力の追加 | `controller_device.cpp` の `InputDef` 表、`resources/input/vhmd_controller_profile.json`、必要ならリマッピング(`tools/gen_remapping.py`)とレガシーバインディング |

## プラットフォーム抽象化(将来の Linux 対応)

- `util/platform.h`: `VHMD_EXPORT`(`__declspec(dllexport)` / `visibility("default")`)、`kPathSeparator`。
- `net/udp_socket.cpp`: WinSock と BSD ソケットを `#if` で切替。ヘッダは OS 非依存(ハンドルは `intptr_t`)。
- `util/precise_sleep.cpp`: Windows は高分解能 Waitable Timer(`timeBeginPeriod` によるシステム全体のタイマ変更はしない)、他は `sleep_until`。
- CMake: `CMAKE_CXX_VISIBILITY_PRESET hidden`、出力先 `bin/<win64|linux64|osx64>/driver_vhmd.<dll|so>`、`PREFIX ""`。
- 設定ファイルパスは `std::filesystem::u8path` で UTF-8 として扱う。
- Linux: WSL2 でビルド・単体テスト済み。ELF では `driver_exports.map` で `HmdDriverFactory` 以外を非公開にし、libstdc++/libgcc を静的リンク。Linux 版 SteamVR での動作は未検証(`docs/linux.md`)。

## SteamVR に登録する名前・ID

| 種別 | 値 |
|---|---|
| ドライバ名 | `vhmd`(`driver.vrdrivermanifest`) |
| コントローラタイプ | `vhmd_hmd`, `vhmd_controller`, `vhmd_tracker`(独自。他社名は使用しない) |
| メーカー名 | `VirtualHMD (development)` |
| トラッキングシステム名 | `vhmd` |
| レンダーモデル | SteamVR 同梱の汎用モデル `generic_hmd` / `generic_controller` / `generic_tracker` |
| 登録デバイスタイプ | `vhmd/<serial>`(トラッカーロールの設定キー `/devices/vhmd/<serial>` に対応) |
