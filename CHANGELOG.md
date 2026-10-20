# 変更履歴

## 未リリース

### 追加
- **ヘッドセット側アプリ VHMD Link(段階 1)**: Pico 4 Ultra などの単体型ヘッドセットで動く OpenXR のネイティブアプリ(`headset/`)。頭・両手の姿勢と入力を USB(`adb reverse`)経由で送り、PC の `python -m vhmd_client.headset_bridge` が VHMD ドライバに渡す。Gradle を使わない `build_apk.py` でビルドする。
- **VHMD Link: 表示のなめらかさ**。復号した画像をできた時点で受け取ってキューに入れる(描画ループが取りに行く前に上書きされ、毎秒 9 枚ほど失っていた)。`headset_bridge --buffer 1` で 1 枚ためて表示し、72 Hz の表示のほぼ全部を新しい画像にできる(遅れは約 12 ms 増える)。ヘッドセットが待ち時間・表示の抜けを PC に報告する。
- **VHMD Link: 映像を 72 fps に**。Windows のダイレクトモードで、プレビューのフレームレート上限を「前回から 1 周期以上」から締め切り方式に変更(表示と同じレートだと揺らぎで約 3 割落ちていた)。PC 側は共有メモリの読み出しと ffmpeg への書き込みを別スレッドで並行させる。ヘッドセットの受信・復号・表示の状況を 2 秒ごとに PC に送り、`headset_bridge` に表示する。
- **VHMD Link: 頭の動きの補正と、表示の合わせ込み**。ドライバがプレビューのフレームごとに描画時の頭の姿勢を書き(ヘッダーの後ろに追加、読み手は互換)、ヘッドセットはその姿勢で表示レイヤーを置くので、Pico の合成器が今の頭の姿勢との差を補正する。ヘッドセットが自分の視野角・解像度・IPD・リフレッシュレートを送り、`python -m vhmd_client.mode vhmd --headset` で VHMD をそれに合わせる。ドライバは目ごとの非対称な視野角(`vhmd_display` の `leftTanLeft` など)に対応。
- **VHMD Link: Wi-Fi で接続**。アプリが LAN のブロードキャストで PC の `headset_bridge` を見つけ、姿勢・入力は UDP、映像は TCP で送る。見つからなければ USB(`adb reverse`)に切り替える。`pc_address.txt` でアドレスを固定できる。
- **VHMD Link(段階 2): VHMD の映像をヘッドセットに表示**。PC の `headset_stream` がダイレクトモードのプレビューを ffmpeg(GPU エンコーダー)で H.264 にして USB 経由で送り、ヘッドセットはハードウェアデコーダーの出力をコピーなしでテクスチャにして表示する。
- **VHMD と実機 HMD の切り替え(Windows)**: `python -m vhmd_client.mode pico|vhmd`、FPS 操作ツールのボタン。SteamVR の設定 `driver_pico.enable` と `driver_vhmd.configFile`(インストール済みの設定から作るプロファイル)を切り替えて SteamVR を再起動する。SnapVR などが SteamVR をすぐ起動し直す環境でも、元の SteamVR だけを止めて待つ。
- **キーボードとマウスでの操作(Windows)**: `python -m vhmd_client.fps`。マウスで視点、WASD で移動、マウスボタンとキーでコントローラーの入力。各デバイスの位置・角度の表示と数値入力、上から見た図でのドラッグ、SteamVR 上の実際の姿勢の表示(pyopenvr がある場合)、送信の一時停止と SteamVR の姿勢からの引き継ぎ、今の姿勢を Python のコードとしてコピー。
- FPS 操作ツールの画面を整理: Windows のライト/ダークに合わせたテーマ(`--theme`)、「デバイス」「操作」「上から見た図」「HMD の視点」「キー操作」のパネル、モードと送信状態の表示、位置・角度をまとめた表、頭を中心にした地図(視野の扇形、縮尺、凡例、重ならないラベル)。
- FPS 操作ツールに HMD の視点を表示(ダイレクトモード)。プレビューを GDI で直接描くため、表示が UI の動きを妨げない。
- 姿勢を専用スレッドから一定間隔(既定 120 回/秒)で、速度・角速度付きで送るようにした。SteamVR が送信の合間を補間するので、カクつきが減る。止まったときは速度を 0 にして行き過ぎを防ぐ。「動きの予測」でオン・オフでき、既定は VHMD モードでオフ、実機 HMD に追従する Pico モードでオン。
- FPS 操作で Raw マウス入力(Windows のマウス加速を受けない)を既定にした。チェックボックスでカーソル移動量に戻せる。
- HMD の視点の表示がウィンドウの大きさに追従する。共有メモリの映像をコピーせずに GDI で切り抜き・縮小するので、大きく表示しても UI が重くならない。`PreviewReader.address` / `peek` / `sequence`。
- HMD の視点の表示モード: 片目 16:9 / 両目 / 片目(全体)。`PreviewReader.read_scaled` に両目(`eye="both"`)と縦横比での切り抜き(`aspect`)を追加。
- `math3d.to_euler`(`from_euler` の逆変換)と `math3d.conjugate`。`PreviewReader.read_scaled`(片目を縮小して、必要な画素だけ読む)。

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
