# 変更履歴

## 未リリース

### 修正
- VMT の Joint / Follow で親を持つトラッカーが 0.5 秒で無効になる問題。親を持つ姿勢はタイムアウトしないようにした(VMT と同じ)。

### テスト
- `openxr_check.py`: OpenXR の Vive トラッカー拡張で VHMD のトラッカーが見えるかを情報として表示。コントローラーが無効な構成ではハンドトラッキングの確認を飛ばす。
- `stress_restart.py`: vrmonitor が遅れて起動しても SteamVR を止められるようにし、ログはその回の vrserver の PID でセッションを探すようにした。

### ドキュメント
- 実機 HMD(Pico 4 Ultra + PICO Connect)にトラッカーを足す構成を確認し、「実機 HMD と併用するとき」を書き直した。
- 実機併用での VMT、30 分の連続稼働、実行中のアンインストール、VRChat(ダイレクトモード)の結果を追記。
- Pico 4 Ultra 向けアプリの調査メモ(`docs/pico4ultra_research.md`)。

## v0.5.0(2026-10-20)

### 追加
- **HMD の装着センサー**: `input` の `proximity`(既定は装着中、設定 `hmd.proximity`)。アプリには `k_EButton_ProximitySensor`、`/user/head/proximity`、OpenXR の `XR_EXT_user_presence` として届く。
- **バッテリー表示**: 全デバイスで `battery`(0〜1)と `charging`。コントローラー・トラッカーは既定で満充電。Python の `VHMDClient.battery()`、`proximity()`。
- **入力の追加**: コントローラーにトラックパッド(位置・クリック・タッチ・force)、グリップの force、サムレストのタッチ。トラッカーにシステム・メニュー・グリップ・トリガー・トラックパッド。
- **姿勢の加速度とサンプルの遅れ**: `pose` の `acc` / `angacc` と `age`(SteamVR の `poseTimeOffset`)。
- **VMT 互換 OSC を VMT 0.15 の API に拡張**: Joint / Follow(親デバイスに追従)、スケルトン(ボーン + Apply、Scalar)、ルーム行列、Reset、購読と `/VMT/Out/*` の出力(127.0.0.1:39571)、振動の `/VMT/Out/Haptic`、トラックパッドとグリップの force の割り当て。
- 設定: プレイエリアの大きさ(`hmd.playArea`)、SteamVR が切り替えるオーディオデバイス(`hmd.audio`)。
- 入力名の多言語化(英語・日本語・中国語・韓国語)。

### 修正
- OpenXR の LOCAL / LOCAL_FLOOR 空間で HMD が上下逆さまになる問題。vhmd のデバイスにトラッキングユニバースが無く、SteamVR の着座原点が全ゼロの行列になっていた。全デバイスにユニバース ID を設定し、HMD がドライバ提供のチャペロン(原点 = 原点、境界は非表示)を渡すようにした。

### テスト
- 視線がアプリまで届くことを確認する統合テストを追加: OpenVR(`IVRInput` の視線取得、`eye_check.py`)と OpenXR(`XR_EXT_eye_gaze_interaction`、描画なしのセッション、`openxr_eye_check.py`)。統合テストは 58 項目に(ユニバースと較正、OpenXR の LOCAL 空間、装着センサー、バッテリー、追加の入力、サンプルの遅れ、OpenXR のハンドトラッキングと装着状態を含む。VMT 互換 OSC を有効にすると 9 項目追加)。

### ドキュメント
- 視線の座標空間を確定: ドライバに渡す origin / target は HMD ローカル空間(SteamVR が HMD の姿勢を掛けてアプリに渡す)。
- 表情(フェイシャルトラッキング)の調査結果と、対象外とした判断を記録(`docs/face_tracking_research.md`)。

## v0.4.1(2026-10-20)

### 変更
- **インストール時に既存の環境を記録**: `install_driver` が登録前に SteamVR の設定ファイル(`openvrpaths.vrpath`、`steamvr.vrsettings`、`chaperone_info.vrchap`)、登録済みドライバの一覧、SteamVR のビルド番号を記録する(Windows: `%LOCALAPPDATA%\VHMD_DevKit\history`、Linux: `~/.local/state/vhmd_devkit/history`)。
- **アンインストールの整理**: 既定で、登録解除に加えてドライバが作ったファイルと SteamVR 設定の vhmd の項目を削除し、記録からのそれ以外の差分を表示する。`-Restore` / `--restore` で記録した状態に戻せる。解除の直前の状態も記録するので、解除自体も元に戻せる。`-CleanSettings` は不要になった(互換のため受け付ける)。

## v0.4.0(2026-10-19)

### 追加
- **Windows のダイレクトモード**(`display.mode = "direct"`): SteamVR がデスクトップにウィンドウを開かず、ドライバが D3D11 の共有テクスチャで映像を受け取る。アプリは設定どおりのフレームレートで動く(ウィンドウモードは 66 fps 止まりだった)。
- **プレビューウィンドウ**(Windows のダイレクトモード): `display.previewWindow` または `preview_window` 操作で表示 / 非表示。閉じても SteamVR は止まらない。
- **フレームレートの指定**: `display.refreshHz`(1〜240、既定 90)。ダイレクトモードでは実行中に `refresh_rate` 操作で変更できる。
- **映像の取り出し**(Windows / Linux 共通): 共有メモリへの書き出しを `preview` 操作で開始・停止。Python の `PreviewReader` が Windows の名前付きマッピングにも対応。
- **コントローラー ⇄ ハンドトラッキングの切り替え**: `hand_mode` 操作。SteamVR と VRChat を止めずに切り替わる。設定 `controllers.handMode`。
- **SteamVR 上のラベル表示**: トラッカーのモデルとステータスアイコンに部位(`L FOOT` など)と ID、コントローラーに `L` / `R` と ID。ロールの変更に追従。
- コントローラーのレンダーモデルに指先などの部品(`tip`、`handgrip`、`openxr_aim` など)。
- `status` に `perf`(ドライバ自身のスレッドの CPU と 1 回の処理時間)、`hand_mode`。
- テスト: 長時間の安定性試験 `soak_test.py`、`linux_check.py` に終了前の描画確認とプレビューの一時停止確認、ハンドモードの統合テスト。配布アーカイブに検証スクリプトを同梱。

### 変更
- コントローラーの骨格の既定を `partial` に(実機のコントローラーと同じ扱い。VRChat でボタン操作が使える)。
- Linux のプレビューは既定で停止し、ツールが頼んだときだけ書き出す(`preview.autoStart` で従来どおりにできる)。

### 軽量化
- 変化のない姿勢は 250 ms ごとの再送に、待機中のデバイスは 1 秒ごとに、ボタンから作る指の骨格は値が変わったときだけ計算(待機時の 1 回の処理 42 → 9 µs)。
- Windows のダイレクトモード 90 Hz で、SteamVR とアプリの CPU 合計がウィンドウモードの約半分(38.8 % → 18.4 %)、30 Hz では 12.7 %。

### 修正
- ドライバの UDP ソケットと共有イメージのハンドルが子プロセスに継承され、vrserver の異常終了後にポートが塞がったままになる問題。
- Linux のキャプチャセルフテストが、最初の Vulkan デバイスが共有イメージを扱えない環境(WSL など)で失敗していた問題。

## v0.3.0(2026-10-19)

- Linux のダイレクトモード(`IVRDriverDirectModeComponent`、共有 Vulkan イメージ)。実ディスプレイや DRM リースが無くてもコンポジタが動く。
- 共有メモリへのプレビューと `preview_viewer.py`。
- `linux_check.py`(Linux の実機確認とレポート出力)。
- 実 GPU の Linux 版 SteamVR(RTX 4000 Ada)で 90 fps の描画と統合テストを確認。

## v0.2.0

- 3 点(HMD + 左右コントローラー)を基本に、トラッカーは最初に送ったときに追加(オンデマンド)。
- Windows / Linux のビルド済みバイナリを GitHub Releases で配布。
- ドキュメントを日本語・英語・中国語・韓国語で整備。BSD 3-Clause ライセンス。
