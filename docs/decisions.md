# 判断・仮定・非対応事項の記録

一次情報は `third_party/openvr` (ValveSoftware/openvr **v2.15.6**) の `headers/openvr_driver.h` と `samples/drivers/`、およびこの PC にインストールされている **SteamVR 2.17.10 (buildid 25330290)** の同梱ファイル・ログです。確認できなかった点は「未検証」と明記しています。

## 1. 確認事項(要件 13 章)への回答 — 仮定で進めたもの

| # | 質問 | 採用した仮定 | 状態 |
|---|---|---|---|
| 1 | 通信方式 | UDP+JSON。VMT 互換 OSC も同じポートで実装(既定 OFF) | 実装・検証済み |
| 2 | トラッカー台数 | 8 台(最大 16、設定で変更) | 実装・検証済み |
| 3 | 視線の送信先 | ヘッダが `CreateEyeTrackingComponent` に対応していたので SteamVR 経由で実装 | 実装済み・アプリ側の受信は**未検証** |
| 4 | VRChat の GUI 操作 | 不可。手動検証手順書で代替 | `docs/verification.md` |
| 5 | openvr のバージョン固定 | 検証時点の最新タグ v2.15.6 を固定(`VHMD_OPENVR_TAG`) | 済み |

## 2. API に関する判断(根拠付き)

| 事項 | 判断 | 根拠 |
|---|---|---|
| ディスプレイ | `IVRDisplayComponent`(デスクトップウィンドウ方式、歪み無し、`DisplayDebugMode=true`) | `samples/drivers/drivers/simplehmd` と同方式 |
| HMD の `shouldApplyHeadModel` | 既定 `false`(`hmd.headModel` で切替) | `true` だと回転時に位置が最大 93 mm ずれることを実測。プロトコルの `pos` は頭の位置と定義 |
| 起動直後のポーズ送信 | `Activate` から 100 ms 待つ | 待たないと vrserver.txt に `PoseUpdated from driver vhmd for device index N which is owned by driver (-1)` が出ることを実測 |
| 視線 | `CreateEyeTrackingComponent(container, "/eyetracking")` + `Prop_SupportsXrEyeGazeInteraction_Bool` | ヘッダに API あり。パス名 `/eyetracking` は SteamVR 同梱 `driver_vrlink.dll` 内の文字列で確認(公式ドキュメントでは未確認)。`VREyeTrackingData_t` の座標空間はヘッダに記載なし → HMD ローカル空間と仮定 |
| 指スケルトン | `CreateSkeletonComponent`、既定 `VRSkeletalTracking_Full` | サンプル `handskeletonsimulation` と同じ。ボーン生成はサンプルの `hand_simulation.cpp` を third_party からそのままコンパイル |
| スケルトンの aux ボーン | 長さ 0 のクォータニオンは単位クォータニオンに置換 | サンプルの手モデルは aux ボーン 26〜30 を 0 のまま返すことを検証中に発見 |
| トラッカーロール | SteamVR 設定 `trackers` セクションに `/devices/vhmd/<serial>` → `TrackerRole_*` を書き込む(既存値があれば上書きしない)。`Prop_RegisteredDeviceType_String = vhmd/<serial>` | 書き込み後に steamvr.vrsettings に反映されたことを確認。SteamVR の「トラッカーの管理」画面での表示は**未検証(GUI)** |
| コントローラの互換性 | 独自タイプ `vhmd_controller` + リマッピング(oculus_touch / knuckles / vive_controller から)。`simulate_controller_type` 等は全て `false` | 他社製品へのなりすまし禁止(要件)。SteamVR ログで「Parsing remapping data succeeded」「auto-remapped binding file from knuckles to vhmd_controller」を確認 |
| `compatibility_mode_controller_type` | 使わない | PICO ドライバ等はこれで oculus_touch を名乗るが、なりすましになるため不採用 |
| レガシー入力 | 独自の `legacy_bindings_vhmd_controller.json`(joystick=axis0, trigger=axis1, grip=axis2) | SteamVR 汎用バインディングには A/B が無いため |
| アイコン | OpenVR サンプルのアイコンを同梱(BSD-3-Clause) | |
| レンダーモデル | SteamVR 同梱の `generic_hmd` / `generic_controller` / `generic_tracker` | 他社製品モデルは使わない |
| 更新周期 | 既定 120 Hz、Windows は高分解能 Waitable Timer | `timeBeginPeriod` はシステム全体に影響するため不使用 |
| ログ | `VRDriverLog()` 経由で vrserver.txt、不正パケット警告は 5 件/秒のトークンバケット | |
| 静的 CRT (/MT) | 採用 | VC++ 再頒布可能パッケージ非依存。DLL の依存は `WS2_32.dll`, `KERNEL32.dll` のみ(dumpbin で確認) |

## 3. 未対応・制約

| 項目 | 状態 / 理由 | 将来の案 |
|---|---|---|
| 実画面表示 | 対象外(ダミーディスプレイ)。SteamVR はデスクトップ上にウィンドウを開く | `IVRDriverDirectModeComponent` / `IVRVirtualDisplay` の実装に差し替え(`HmdDevice::GetComponent`) |
| 視線のアプリ側確認 | OpenVR クライアント API に視線を読む手段がなく自動検証できない | OpenXR の `XR_EXT_eye_gaze_interaction` で読むテストアプリ、または VRChat OSC のアイトラッキングパラメータ(`/tracking/eye/...`)を直接送る別ツール |
| トラッカーのみモード (`hmd.enabled=false`) | ドライバが HMD を追加しないことは確認。この PC では実ヘッドセット(PICO 4S)が未接続のため、SteamVR に HMD が無く他デバイスも有効化されない → **未検証** | 実 HMD 接続時に `docs/verification.md` の手順で確認 |
| VMT の Joint / Follow / Skeleton / ルーム行列 | 未実装(破棄してログ) | `protocol/osc.cpp` に追加 |
| 実行時のデバイス追加・削除 | OpenVR はデバイス削除 API が無いため、台数は起動時の設定で決定 | `TrackedDeviceAdded` は後からも呼べるので「最初のパケットで追加」モードは追加可能 |
| Linux の表示経路 | v0.2.0 を Linux 版 SteamVR 2.17.10(Arch、KDE Wayland)で試した結果、ドライバは読み込まれ HMD も認識されたが、コンポジタが `CannotDRMLeaseDisplay` で起動しなかった(null ドライバも同じ。Valve issue #889)。`IVRVirtualDisplay` は Linux で壊れている(issue #409)。**v0.3.0 で `IVRDriverDirectModeComponent` を実装**(ALVR の Linux ドライバと同じ方式。共有 Vulkan イメージを `IVRIPCResourceManagerClient` で確保し、vsync はドライバが通知)。**Linux 版 SteamVR 上では未検証** | `tests/integration/linux_check.py` で確認。ALVR の動作確認が SteamVR の安定版かベータ版かは未確認 |
| 共有イメージの読み取り | 作成時と同じパラメータで OPAQUE_FD としてインポートし、最初の 1 回だけ UNDEFINED から遷移(ALVR と同じ扱い)。同期は dma-buf の書き込みフェンスを上限付きで待つ。プレビュー用途なので、間に合わなければ欠けたフレームになるだけ | — |
| libvulkan への依存 | 実行時に `dlopen`。Vulkan が無い環境でもドライバは読み込まれ、プレビューだけが無効になる | — |
| Linux の配布性 | `.so` はビルド環境の glibc 以上を要求(Ubuntu 24.04 ビルドで 2.38)。libstdc++/libgcc は静的リンク | 対象 PC でビルド、または古い環境/Steam Runtime SDK でビルド |
| macOS | 対象外 | |
| 送信→反映の遅延 | 更新スレッド周期待ち(120 Hz で最大 8.3 ms) | 受信スレッドから即時 `TrackedDevicePoseUpdated` するモード |
| `steam.client (vhmd_*) has no configured binding` | SteamVR が Steam クライアント等のアプリ向けに出すログ。PICO 等の実機でも同じ行が出ている。動作に影響なし | — |

## 4. この PC 固有の注意

- **PICO Connect のドライバも登録されている**。今回の検証時はヘッドセット未接続で PICO 側が HMD を追加しなかったため、vhmd の HMD がアクティブ HMD になった。PICO 接続時に両方が有効だとどちらがアクティブになるかは**未検証**。実機を使うときは `tools/uninstall_driver.ps1` で登録解除するか、`hmd.enabled: false`(トラッカーのみ)にする。
- SteamVR の `LastKnown.ActualHMDDriver` が `vhmd` に変わる(SteamVR が自動で記録する情報。PICO 接続時には PICO に戻る想定)。
- **SnapVR (`snapvr-engined.exe`) が SteamVR 終了後に SteamVR を再起動する**。テストスクリプトの `--stop-relaunchers` はこれを検出して終了させる(SteamVR 本体・Steam クライアント・ゲームは対象外)。同時に OpenVR-AdvancedSettings も終了対象になる。いずれも次回 SteamVR 起動時に自動起動するオーバーレイ。
- vrmonitor は `taskkill`(WM_CLOSE)では終了しない。テストではマスタープロセス (vrmonitor) を終了させ、vrserver の「Lost master process ... Quitting all」→「Graceful exit」経路で終了させている(ドライバの `Cleanup` が呼ばれることをログで確認)。

## 5. 不採用にしたもの

- 他社コントローラタイプ名・レンダーモデル・シリアル形式の流用、`simulate_*`、アンチチート回避に繋がる機能。
- SteamVR / Steam 本体ディレクトリへのファイルコピー(登録は `vrpathreg` のみ)。
- `steamvr.vrsettings` の直接編集(例外: アンインストールの `-CleanSettings` オプションで、バックアップを取ったうえで vhmd 由来の項目のみ削除)。
