**日本語** | [English](en/architecture.md) | [简体中文](zh-CN/architecture.md) | [한국어](ko/architecture.md) — [README に戻る](../README.md)

# アーキテクチャ

## 全体像

```
[外部クライアント (Python / 任意言語 / VMT送信ツール)]
        │ UDP 127.0.0.1:39570  (JSON or VMT-OSC)
        ▼
┌──────────────────────── driver_vhmd.dll / .so (vrserver プロセス内) ────────────────────────┐
│ UdpReceiver スレッド ──► MessageRouter ──► DeviceState (デバイス毎, mutex 保護)              │
│   net/udp_*              state/router       state/device_state                              │
│   (OS依存はここだけ)      protocol/parser    ・最新ポーズ / 入力 / スケルトン / 視線          │
│                          protocol/osc       ・タイムアウト判定・スムージング                 │
│                                                    │                                       │
│ 更新スレッド (既定120Hz, 高分解能タイマ) ───────────┘                                       │
│   DeviceProvider::UpdateLoop → VirtualDevice::Update(変化があったものだけ SteamVR へ)        │
│     → TrackedDevicePoseUpdated / UpdateBoolean/Scalar/Skeleton/EyeTrackingComponent         │
│                                                                                             │
│ RunFrame (vrserver メインスレッド) → オンデマンド追加、振動イベントを UDP でクライアントへ    │
│                                                                                             │
│ HMD の表示(どちらか):                                                                       │
│   ウィンドウモード … IVRDisplayComponent。コンポジタがデスクトップにウィンドウを開く(Windows)│
│   ダイレクトモード … IVRDriverDirectModeComponent。コンポジタが描いた左右の目をドライバが受け取り、│
│                      vsync もドライバが刻む(Linux: Vulkan、Windows: D3D11)                  │
│                      → プレビューウィンドウ(Windows)/ 共有メモリ(ツール・Python 向け)        │
└─────────────────────────────────────────────────────────────────────────────────────────────┘
        ▼
     SteamVR (vrserver / vrcompositor) → アプリ (VRChat 等)
```

## レイヤ

| レイヤ | ディレクトリ | OpenVR 依存 | 単体テスト |
|---|---|---|---|
| 数学 | `driver/src/math/` | なし | ○ |
| プロトコル(JSON / OSC) | `driver/src/protocol/` | なし | ○ |
| 設定・ロール | `driver/src/config/` | なし | ○ |
| 状態・ルーティング | `driver/src/state/` | なし | ○ |
| ネットワーク | `driver/src/net/` | なし(WinSock/BSD を内部で切替) | ○(ループバック) |
| 表示用ラベル(モデル・アイコン生成) | `driver/src/visuals/` | なし(ドット文字と PNG 書き出しを内蔵) | ○ |
| ユーティリティ | `driver/src/util/` | なし | 一部 |
| デバイス・プロバイダ | `driver/src/*.cpp` | あり | 統合テスト |
| Linux ダイレクトモード | `driver/src/linux/` | あり(+ Vulkan、実行時ロード) | キャプチャはセルフテスト、全体は `linux_check.py` |
| Windows ダイレクトモード | `driver/src/windows/` | あり(+ D3D11 / DXGI) | 統合テスト(`display.mode = direct` で実行) |

OpenVR 非依存部分は静的ライブラリ `vhmd_core` にまとまっており、ドライバ DLL とテスト実行ファイルの両方がリンクします。

## スレッドとライフサイクル

| スレッド | 役割 | 終了 |
|---|---|---|
| vrserver メイン | `Init` / `RunFrame` / `Cleanup`、`Activate` / `Deactivate` | — |
| 受信 | `UdpReceiver::Loop`。100 ms 毎に poll がタイムアウトし停止フラグを確認 | `Cleanup` で `Stop()` → join(最大 ~100 ms) |
| 更新 | `DeviceProvider::UpdateLoop`。固定周期で全デバイスを更新し、1 秒ごとに自分の負荷を計測(`status` の `perf`) | `Cleanup` で停止フラグ → join(最大 1 周期) |
| vsync(ダイレクトモード) | `DirectModeComponent::VsyncLoop`。`refreshHz` の周期で `VsyncEvent` を通知(実行中に変更可) | HMD の `Deactivate` で停止 |
| キャプチャ(Linux) | `FrameCapture::Worker`。頼まれているあいだだけ最新フレームを縮小して共有メモリへ | デバイス破棄時に join |
| プレビューウィンドウ(Windows) | `DirectModeComponent::WindowLoop`。表示中だけ左右の目をウィンドウへ | HMD の `Deactivate` で停止 |
| 読み出し(Windows) | `DirectModeComponent::ReadbackLoop`。頼まれているあいだだけ GPU から読み出して共有メモリへ | HMD の `Deactivate` で停止 |
| コンポジタ(SteamVR 側) | `CreateSwapTextureSet` / `SubmitLayer` / `Present` / `PostPresent` を呼ぶ。`PostPresent` は次の vsync まで待たせる | — |

- `Init`: 設定読込 → 全デバイスの `DeviceState` とデバイスオブジェクトを生成 → **オンデマンドでない**デバイス(既定では HMD と左右コントローラの 3 点)だけ `TrackedDeviceAdded` → 受信・更新スレッド開始。
- オンデマンド追加: 受信スレッドが未登録トラッカー宛てのメッセージを受けると、値をバッファに入れてから `RequestAdd(id)`(キューに積むだけ)。`RunFrame`(vrserver メインスレッド)が `ProcessPendingAdds()` でキューを処理し `TrackedDeviceAdded` を呼ぶ。バッファ済みの値が `Activate` 後 100 ms から送られるので、追加直後から同期している。
- ハンドモード: 左右の手はそれぞれ「コントローラー」と「ハンド」(`VHMD-HAND-L/R`)の 2 デバイスを持ち、同じ `DeviceState` を共有します(ハンド側は初めて `hands` にしたときに追加)。更新スレッドは状態を 1 回だけ読み、両方に渡します(入力やスケルトンは読むと消えるため)。接続中として報告するのは選ばれている方だけです。
- `Activate(index)`: プロパティ・入力コンポーネント作成、表示用ラベル(モデルとアイコン)の生成。デバイス毎の mutex で更新スレッドと排他。
- 更新スレッドは `Activate` 後 100 ms 経つまでそのデバイスのポーズを送りません(vrserver がデバイス所有権を登録し終える前に送ると `PoseUpdated ... owned by driver (-1)` エラーになるため。実測で確認)。
- `Deactivate`: インデックスを無効化 → 以後、更新スレッドはそのデバイスに触れません。
- `Cleanup`: 受信停止 → 更新停止 → デバイス破棄 → ログシンク解除。SteamVR 終了時にハングしないことを 31 回連続の起動終了で確認(`docs/verification.md`)。
- 受信ハンドラ内の例外はすべて捕捉され、受信スレッドは継続します。

## データフロー上の決まり

- 受信側は「最新値で上書き」。キューは持たず、高頻度送信(1000 pps 以上)でも遅延が積み上がりません。
- 入力値はデバイス毎・名前毎に最新値へまとめ、更新スレッドで SteamVR へ反映。
- 遅延の内訳: UDP 受信は即時、SteamVR への反映は次の更新周期(120 Hz なら最大 8.3 ms)。実測の送信→アプリ可視は中央値 8.2 ms(`docs/verification.md`)。
- **無駄な送信をしない**: 変化のない姿勢(速度 0)は 250 ms ごとに再送するだけ。待機中のデバイス(ハンドモードで選ばれていない側)は 1 秒ごと。ボタンから作る指の骨格は値が変わったときだけ計算します。
- **映像は見る人がいるときだけ**: プレビュー(共有メモリ)は `preview` 操作で頼まれたときだけ、プレビューウィンドウは表示中だけ、GPU のコピーと書き出しを行います。
- ソケットと共有イメージのハンドルは子プロセスに継承させません(vrserver が異常終了しても、子プロセスがポートを握り続けないように)。

## 拡張ポイント

| やりたいこと | 触る場所 |
|---|---|
| 新しいデバイス種別 | `VirtualDevice` を継承(`OnActivate` で プロパティ/コンポーネント、`OnInput`/`OnTick` 等をオーバーライド)し、`DeviceKind` と `DeviceProvider::CreateDevices` に追加 |
| 新しいメッセージ型 | `protocol/messages.h` に構造体を追加 → `parser.cpp` の `ParseOne` → `MessageRouter::HandleDatagram` → `DeviceState` → デバイスのフック |
| 新しい `config` 操作 | `router.cpp` の `HandleConfig` に分岐を足し、ドライバ側の処理は `RouterHooks` の関数として `DeviceProvider::Init` で渡す |
| 新しいトランスポート (WebSocket, 共有メモリ等) | `UdpReceiver` と同じ形で受信し `MessageRouter::HandleDatagram()` を呼ぶだけ(ルーターはトランスポート非依存) |
| 別プロトコル互換 | `protocol/osc.*` と同様に `ParseResult` を返すパーサを追加し、`HandleDatagram` の判定に追加 |
| 映像の利用(エンコード / 配信 / 録画) | 左右の目を受け取る場所は Linux が `linux/frame_capture.cpp` の `Capture()`、Windows が `windows/direct_mode.cpp` の `CopyPreview()`。共有メモリの代わりに別の出力を足せる |
| コントローラ入力の追加 | `controller_device.cpp` の `InputDef` 表、`resources/input/vhmd_controller_profile.json`、必要ならリマッピング(`tools/gen_remapping.py`)とレガシーバインディング |
| 表示用ラベルの見た目 | `visuals/device_visuals.cpp`(ドット文字、メッシュ、アイコンの色) |

## プラットフォーム抽象化

- `util/platform.h`: `VHMD_EXPORT`(`__declspec(dllexport)` / `visibility("default")`)、`kPathSeparator`。
- `net/udp_socket.cpp`: WinSock と BSD ソケットを `#if` で切替。ヘッダは OS 非依存(ハンドルは `intptr_t`)。
- `util/precise_sleep.cpp`: Windows は高分解能 Waitable Timer(`timeBeginPeriod` によるシステム全体のタイマ変更はしない)、他は `sleep_until`。
- `util/thread_cpu.cpp`: スレッドの CPU 時間(`GetThreadTimes` / `CLOCK_THREAD_CPUTIME_ID`)。
- ダイレクトモードは同じクラス名・同じ公開関数で `linux/` と `windows/` に別実装があり、`hmd_device.cpp` がプラットフォームに応じて取り込みます。
- CMake: `CMAKE_CXX_VISIBILITY_PRESET hidden`、出力先 `bin/<win64|linux64|osx64>/driver_vhmd.<dll|so>`、`PREFIX ""`。
- 設定ファイルパスは `std::filesystem::u8path` で UTF-8 として扱う。
- Linux: ELF では `driver_exports.map` で `HmdDriverFactory` 以外を非公開にし、libstdc++/libgcc を静的リンク。実 GPU の Linux 版 SteamVR で確認済み(`docs/linux.md`)。

## SteamVR に登録する名前・ID

| 種別 | 値 |
|---|---|
| ドライバ名 | `vhmd`(`driver.vrdrivermanifest`) |
| コントローラタイプ | `vhmd_hmd`, `vhmd_controller`, `vhmd_tracker`(独自。他社名は使用しない) |
| メーカー名 | `VirtualHMD (development)` |
| トラッキングシステム名 | `vhmd` |
| トラッキングユニバース | `0x56484D44`(全デバイス共通)。HMD がドライバ提供のチャペロン(着座・立位の原点 = 原点、2 × 2 m、境界は非表示)を渡す |
| シリアル | `VHMD-HMD-0`、`VHMD-CTL-L/R`、`VHMD-HAND-L/R`(ハンドモード)、`VHMD-TRK-0`〜 |
| レンダーモデル | HMD は SteamVR 同梱の `generic_hmd`。コントローラーとトラッカーはドライバが起動時に生成するラベル付きモデル(`resources/rendermodels/vhmd_<serial>_<label>/`、指先などの部品付き)。生成できないときは `{vhmd}vhmd_controller` / `generic_tracker` |
| ステータスアイコン | コントローラー・トラッカーは短い記号入り(`resources/icons/labels/`、起動時に生成) |
| 登録デバイスタイプ | `vhmd/<serial>`(トラッカーロールの設定キー `/devices/vhmd/<serial>` に対応) |
