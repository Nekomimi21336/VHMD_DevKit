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
| ディスプレイ | Windows の既定は `IVRDisplayComponent`(デスクトップウィンドウ方式、歪み無し、`DisplayDebugMode=true`)。`display.mode = direct` と Linux では `IVRDriverDirectModeComponent` | ウィンドウ方式は `samples/drivers/drivers/simplehmd` と同じ。ダイレクトモードは ALVR と同じ考え方(コードは流用していない) |
| Windows のダイレクトモード | D3D11 で `D3D11_RESOURCE_MISC_SHARED` のテクスチャを作り、共有ハンドルを渡す。`Present` で同期テクスチャのキー付きミューテックスを取得・解放。`Prop_GraphicsAdapterLuid_Uint64` にテクスチャのアダプタを設定 | ウィンドウ方式ではアプリが 66 fps で頭打ち、compositor の CPU が高かった。ダイレクトモードは 90 fps で負荷が約半分(実測) |
| フレームレート | ダイレクトモードではドライバの vsync が周期を決める。`refreshHz`(1〜240、既定 90)、実行中は `refresh_rate` 操作で変更し `Prop_DisplayFrequency_Float` も合わせる | コンポジタは 1〜240 Hz に正確に従う。10 Hz 未満ではアプリが 12〜15 fps より遅くならない(SteamVR の待ちの打ち切り)ことを実測 |
| HMD の `shouldApplyHeadModel` | 既定 `false`(`hmd.headModel` で切替) | `true` だと回転時に位置が最大 93 mm ずれることを実測。プロトコルの `pos` は頭の位置と定義 |
| 起動直後のポーズ送信 | `Activate` から 100 ms 待つ | 待たないと vrserver.txt に `PoseUpdated from driver vhmd for device index N which is owned by driver (-1)` が出ることを実測 |
| 視線 | `CreateEyeTrackingComponent(container, "/eyetracking")` + `Prop_SupportsXrEyeGazeInteraction_Bool` | ヘッダに API あり。パス名 `/eyetracking` は SteamVR 同梱 `driver_vrlink.dll` 内の文字列で確認(公式ドキュメントでは未確認)。`VREyeTrackingData_t` の座標空間はヘッダに記載なし → HMD ローカル空間と仮定 |
| 指スケルトン | `CreateSkeletonComponent`。コントローラーは既定 `VRSkeletalTracking_Partial`、ハンド側(ハンドモード)は `Full` | VRChat は Full をハンドトラッキング(メニューはジェスチャー)、Partial をコントローラー(ボタン操作)として扱うことを実測。ボーン生成はサンプルの `hand_simulation.cpp` を third_party からそのままコンパイル |
| コントローラー ⇄ ハンドの切り替え | 手ごとにコントローラーとハンドの 2 デバイスを持ち、片方だけを接続中として報告(`hand_mode` 操作) | OpenVR にはデバイスを外す API が無く、骨格の精度も作成後に変えられないため。SteamVR と VRChat を止めずに切り替わることを確認 |
| スケルトンの aux ボーン | 長さ 0 のクォータニオンは単位クォータニオンに置換 | サンプルの手モデルは aux ボーン 26〜30 を 0 のまま返すことを検証中に発見 |
| トラッカーロール | SteamVR 設定 `trackers` セクションに `/devices/vhmd/<serial>` → `TrackerRole_*` を書き込む(既存値があれば上書きしない)。`Prop_RegisteredDeviceType_String = vhmd/<serial>` | 書き込み後に steamvr.vrsettings に反映されたことを確認。SteamVR の「トラッカーの管理」画面での表示は**未検証(GUI)** |
| コントローラの互換性 | 独自タイプ `vhmd_controller` + リマッピング(oculus_touch / knuckles / vive_controller から)。`simulate_controller_type` 等は全て `false` | 他社製品へのなりすまし禁止(要件)。SteamVR ログで「Parsing remapping data succeeded」「auto-remapped binding file from knuckles to vhmd_controller」を確認 |
| `compatibility_mode_controller_type` | 使わない | PICO ドライバ等はこれで oculus_touch を名乗るが、なりすましになるため不採用 |
| レガシー入力 | 独自の `legacy_bindings_vhmd_controller.json`(joystick=axis0, trigger=axis1, grip=axis2) | SteamVR 汎用バインディングには A/B が無いため |
| アイコン | HMD は OpenVR サンプルのアイコン(BSD-3-Clause)。コントローラー・トラッカーは起動時に生成する記号入りアイコン(`LF`、`W`、`L` など) | どのトラッカーがどの部位か、ステータス画面で分からなかったため |
| レンダーモデル | HMD は SteamVR 同梱の `generic_hmd`。コントローラー・トラッカーは起動時に生成するラベル付きの簡素なモデル(部位・`L`/`R`・ID を表示、指先などの部品付き)。ロールが変わると作り直す | 他社製品モデルは使わない。文字はドライバ内蔵のドット文字で描き、外部フォントや画像ライブラリに依存しない |
| 姿勢の送信頻度 | 変化のない姿勢(速度 0)は 250 ms ごとの再送、待機中のデバイスは 1 秒ごと | 毎周期の送信は SteamVR 側の負荷になっていた(待機時の 1 回の処理 42 → 9 µs) |
| 映像の書き出し | 既定では止めておき、`preview` 操作(または `preview.autoStart`)で開始 | 誰も見ていないのに 1 フレーム約 8 MB を毎秒 15 回コピーしていたため |
| 更新周期 | 既定 120 Hz、Windows は高分解能 Waitable Timer | `timeBeginPeriod` はシステム全体に影響するため不使用 |
| ログ | `VRDriverLog()` 経由で vrserver.txt、不正パケット警告は 5 件/秒のトークンバケット | |
| 静的 CRT (/MT) | 採用 | VC++ 再頒布可能パッケージ非依存。DLL の依存は `WS2_32.dll`, `KERNEL32.dll` のみ(dumpbin で確認) |

## 3. 未対応・制約

| 項目 | 状態 / 理由 | 将来の案 |
|---|---|---|
| 実画面表示 | 対象外(ダミーディスプレイ)。Windows の既定ではデスクトップにウィンドウが開く。ウィンドウを出したくない場合は `display.mode = direct` | — |
| 視線のアプリ側確認 | OpenVR クライアント API に視線を読む手段がなく自動検証できない | OpenXR の `XR_EXT_eye_gaze_interaction` で読むテストアプリ、または VRChat OSC のアイトラッキングパラメータ(`/tracking/eye/...`)を直接送る別ツール |
| トラッカーのみモード (`hmd.enabled=false`) | ドライバが HMD を追加しないことは確認。この PC では実ヘッドセット(PICO 4S)が未接続のため、SteamVR に HMD が無く他デバイスも有効化されない → **未検証** | 実 HMD 接続時に `docs/verification.md` の手順で確認 |
| VMT の Joint / Follow / Skeleton / ルーム行列 | 未実装(破棄してログ) | `protocol/osc.cpp` に追加 |
| 実行時のデバイス削除 | OpenVR にはデバイス削除 API が無い。追加は最初のパケットで行う(オンデマンド)。使わなくなったデバイスは切断として報告する | — |
| Linux の表示経路 | v0.2.0 を Linux 版 SteamVR 2.17.10(Arch、KDE Wayland)で試した結果、ドライバは読み込まれ HMD も認識されたが、コンポジタが `CannotDRMLeaseDisplay` で起動しなかった(null ドライバも同じ。Valve issue #889)。`IVRVirtualDisplay` は Linux で壊れている(issue #409)。**v0.3.0 で `IVRDriverDirectModeComponent` を実装**(ALVR の Linux ドライバと同じ方式。共有 Vulkan イメージを `IVRIPCResourceManagerClient` で確保し、vsync はドライバが通知)。実 GPU(RTX 4000 Ada)の Linux 版 SteamVR 2.17.10 で 90 fps の描画と統合テスト合格を確認。GTX 1050 ではコンポジタが GPU 例外で止まる | 実画面のある Linux デスクトップと、プレビューの開始・停止(軽量化後)の実 GPU での動作は未確認 |
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
- `steamvr.vrsettings` の直接編集。例外はアンインストールのみ: 記録を取ったうえで vhmd 由来の項目(`driver_vhmd`、`vhmd_display`、`/devices/vhmd/` のトラッカーロール)を削除するか、`-Restore` でインストール時に記録したファイルに戻す。インストール後にユーザーが変えたそれ以外の設定は、`-Restore` を指定しない限り変更せず一覧表示だけ行う。
