# 変更履歴

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
