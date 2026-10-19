# 検証手順と結果

## 検証環境(2026-10-18 実施)

| 項目 | 値 |
|---|---|
| OS | Windows 11 Pro 10.0.26200 |
| SteamVR | 2.17.10 (buildid 25330290) |
| VRChat | buildid 25841461(インストール済み・本検証では起動せず) |
| OpenVR SDK | v2.15.6 |
| コンパイラ | MSVC 14.44 (VS 2022 Build Tools 17.14), CMake 4.3.0-rc2 |
| Python / pyopenvr | 3.12.7 / openvr 2.12.1401 |
| GPU | AMD Radeon RX 7900 XTX |
| 実 HMD | PICO 4S のドライバが登録済みだが検証中は未接続 |

## 1. 自動検証(要件 10.1)

### 1-1. ビルドと単体テスト

```bash
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

結果: **警告 0(/W4)、単体テスト 36 件すべて成功**(v0.2.0 でオンデマンド追加の 2 件を追加)。対象: クォータニオン演算・スムージング・Unity↔SteamVR 座標変換、JSON パーサ(正常系/不正値/NaN/範囲外/バッチ上限)、OSC パーサ(バンドル/切れたパケット/型違い)、設定ファイル(同梱ファイルが警告なしで読めること、重複 id、外部公開の拒否、16 台上限)、タイムアウト 3 ポリシー、ルーター(ロール別名、status 応答、set_role)、UDP ループバック。

Python クライアントの単体テスト(SteamVR 不要): `python -m pytest tests/python` → **8 件成功**。

#### クリーン環境からのビルド再現

`build/` と `third_party/openvr` を除いたソース一式を短いパスの一時ディレクトリにコピーし、README の手順で configure → build → ctest → `cmake --install --prefix dist` を実行: **成功**(CMake が OpenVR v2.15.6 を自動取得、`dist/vhmd/` に SteamVR 用フォルダ構成が出力)。

⚠ この PC では README どおりの `cmake -S . -B build -A x64` は **失敗** します。CMake 4.3 が最新の Visual Studio 2026 (18) を自動選択し、そちらに C++ ワークロードが入っていないためです(`No CMAKE_CXX_COMPILER could be found`)。`-G "Visual Studio 17 2022"` または `cmake --preset vs2022` で成功します。VS 2022 だけの環境では問題になりません(未確認)。

### 1-2. SteamVR 統合テスト

```bash
python tests/integration/run_integration.py --stop-relaunchers
```

SteamVR を起動 → 検証 → 終了まで自動。結果 JSON は `tests/integration/results/`。
**v0.2.0 で 29/29 項目成功**(2026-10-18 22:02)。v0.1.0 ではコールドスタートで 27/27 項目成功を連続で確認(2026-10-18 21:28, 21:29 の 2 回。それ以前の版でも 22/22 を 3 回連続)。

| # | 項目 | 結果 | 実測値 |
|---|---|---|---|
| 1 | vrserver.txt にドライバロード・UDP 待受ログ | ✅ | `Loaded server driver vhmd`, `listening on udp://127.0.0.1:39570` |
| 2 | vhmd 由来のエラー / リマッピング失敗が無い | ✅ | |
| 3 | HMD 1 台・アクティブ HMD(index 0) | ✅ | `Active HMD set to vhmd.VHMD-HMD-0` |
| 4 | コントローラ 2 台(左右ロール) | ✅ | index 1/2 |
| 5 | 起動直後は 3 点のみ / トラッカーは最初の pose で追加 (v0.2.0) | ✅ | 起動時 0 台 → 送信後 42 ms で 8 台、送ったポーズで開始 |
| 6 | 独自コントローラタイプのみ(なりすまし無し) | ✅ | `vhmd_hmd`, `vhmd_controller`, `vhmd_tracker` |
| 7 | ポーズ往復(全 11 台 × ランダム 5 姿勢、許容 1 mm / 0.5°) | ✅ | 最大誤差 0.000 mm / 0.000° |
| 8 | 送信→アプリ可視の遅延(中央値 ≤ 11 ms) | ✅ | 中央値 8.2 ms、p95 9.0 ms、最大 9.6 ms |
| 9 | 11 台を 1 データグラムで一括更新 | ✅ | |
| 10 | 明示的な無効化と復帰 | ✅ | |
| 11 | タイムアウト(オンデマンドのトラッカー = `disconnect`、0.5 s 無送信) | ✅ | 無効 + 切断表示 |
| 12 | 不正パケット 217 個(NaN, 巨大値, 乱数, 切れた OSC, 300 件バッチ, 65 KB 等) | ✅ | クラッシュ無し、rejected カウンタ増加 |
| 13 | 1000 pps × 5 秒 | ✅ | 5000 パケット、ドライバ側受信レート合計 1000 Hz、応答継続 |
| 14 | 負荷後もポーズ反映 | ✅ | |
| 15 | アプリから見たポーズ更新レート ≥ 90 Hz | ✅ | 119.5 Hz |
| 16 | コントローラ入力がアプリに届く(レガシー API, 8 ケース) | ✅ | trigger 値/クリック自動閾値、grip、joystick、A、B(secondary)、X(primary)、生パス |
| 17 | アプリの振動要求が UDP クライアントへ返る | ✅ | `{"type":"haptic","id":"right_hand","amplitude":0.75,...}` |
| 18 | スケルトン入力アクションが両手で有効 | ✅ | SteamVR Input (アクションマニフェスト) |
| 19 | `curl` で指の曲げがアプリに反映 | ✅ | 人差し指 curl 0.02 → 0.96 |
| 20 | `bones` の回転がそのまま届く | ✅ | 誤差 0.000° |
| 21 | SteamVR Input アクション経由の trigger 値 | ✅ | |
| 22 | スケルトン未送信時、trigger/grip から指を自動生成 | ✅ | curl 0.96 |
| 23 | SteamVR 終了でドライバ `Cleanup` が走り vrserver が終了 | ✅ | 約 6 s(オーバーレイ終了待ち含む) |

※ 入力系(16〜22)は「フォーカスを持つ Scene アプリ」でしか読めないため、別プロセス(`input_check.py`, `skeleton_check.py`)で `WaitGetPoses` を回しながら実施しています。

### 1-3. 追加で確認したこと(手動実行・ログ確認)

| 項目 | 結果 |
|---|---|
| VMT 互換 OSC `/VMT/Room/Unity` → tracker_0 | ✅ Unity (0.3, 1.1, 0.5) → SteamVR (0.3, 1.1, -0.5) |
| VMT `enable=0` でポーズ無効化 | ✅ |
| トラッカーロールが SteamVR 設定に保存される | ✅ `trackers` セクションに `/devices/vhmd/VHMD-TRK-0: TrackerRole_Waist` ほか 8 件 |
| 2 回目以降の起動で既存ロールを上書きしない | ✅ `keeping existing role` |
| `hmd.enabled=false`(トラッカーのみ) | ⚠ ドライバは HMD を追加せず 10 台を登録。ただし実 HMD 未接続のため SteamVR がデバイスを有効化せず、それ以上は**未検証** |
| DLL のエクスポート | ✅ `HmdDriverFactory` のみ |

### 1-4. 起動/終了の繰り返し

```bash
python tests/integration/stress_restart.py --cycles 30 --stop-relaunchers --relaunch-watch 2 --settle 1
```

各サイクル: SteamVR 起動 → ドライバ応答待ち → 全デバイスに index が付いたか → ポーズ送信 → 終了 → `vhmd: shutting down` と `[vrserver] Graceful exit` がログにあり、クラッシュ/ドライバエラーの行が無いこと。

結果: **31 回連続で全サイクル成功**(25 回は通常設定 約 20 s/回、6 回は短縮設定 約 12.7 s/回)。全回で 11 台に index が付き、`Cleanup` と `Graceful exit` を確認、クラッシュ・ドライバエラーの行なし。
要件の 100 回は、依頼者と相談のうえ 31 回で十分と判断して打ち切りました(100 回は**未実施**)。

### 1-5. 配布アーカイブ (v0.2.0)

`python tools/package_release.py --version 0.2.0 --win64 build/driver/vhmd --linux64 <Linux ビルドの driver/vhmd>`

| 対象 | 確認内容 | 結果 |
|---|---|---|
| win64.zip | 展開 → 同梱 `tools/install_driver.ps1`(引数なし)で展開先の `vhmd` が登録 → SteamVR 起動 → 展開先の DLL がロード、起動時 3 点、足 2 本を送ると 5 点 → 同梱 `uninstall_driver.ps1` で解除 | ✅ |
| linux64.tar.gz | WSL で展開: `.sh` と `.so` に実行権限、CRLF なし、同梱 `install_driver.sh` が展開先の `vhmd` を選択(偽の SteamVR 環境)、Python クライアントが読み込める | ✅(実 SteamVR では未検証) |

### 1-6. アンインストール

`tools/uninstall_driver.ps1 -CleanSettings` を実行し、前後の `steamvr.vrsettings` を比較: vhmd 由来のトラッカーロール 8 件だけが削除され、**それ以外の内容は完全に一致**。外部ドライバ登録も元の 2 件(PICO, AMD)に戻ることを確認。その後 `install_driver.ps1` で再登録済み。

## 1.7 Linux(v0.3.0 ダイレクトモード)

### 外部の Linux 環境での実行(v0.2.0、2026-10-18、協力者による)

| 項目 | 結果 |
|---|---|
| 環境 | Arch Linux、KDE Plasma(Wayland)、SteamVR 2.17.10、glibc 2.44 |
| `tools/verify_env.sh` / `install_driver.sh` | ✅ SteamVR を検出し登録成功(SteamVR 本体へのコピーなし) |
| ドライバのロード、`Using existing HMD vhmd.VHMD-HMD-0` | ✅ |
| コントローラのリマップ(knuckles → vhmd_controller)、レガシーバインディング | ✅ |
| vrcompositor | ❌ `VRInitError_Compositor_CannotDRMLeaseDisplay` で起動せず(`SDL_VIDEODRIVER=x11` でも同じ)。SteamVR 同梱の null ドライバでも 18 回中 18 回失敗 |

→ Linux 版 SteamVR はウィンドウ方式の仮想ディスプレイでは動かないと判断し、v0.3.0 でダイレクトモード(`IVRDriverDirectModeComponent`)を実装しました([linux.md](linux.md))。

### この PC での確認(v0.3.0、WSL2 Ubuntu 24.04)

| 項目 | 結果 |
|---|---|
| Linux ビルド(gcc 13.3、警告 0)、単体テスト 37 件 | ✅ |
| AddressSanitizer / ThreadSanitizer での単体テストとキャプチャセルフテスト | ✅ 指摘なし |
| キャプチャセルフテスト(Mesa llvmpipe の Vulkan で、SteamVR と同じパラメータの共有イメージを作り fd で渡す) | ✅ インポート → 2 倍縮小 → 共有メモリ。左右の画素値が一致(255,0,0 / 0,0,255)、テクスチャ範囲指定、破棄済みハンドル、終了時のファイル削除 |
| `.so` の公開シンボル / 依存 | ✅ `HmdDriverFactory` のみ / `libc`、`libm`、`ld-linux`(libvulkan は `dlopen`) |
| Windows への影響(統合テスト 29 項目、ウィンドウ方式のまま) | ✅ |
| **Linux 版 SteamVR 上でのダイレクトモード** | ❌ 未検証(`tests/integration/linux_check.py` で確認予定) |

### WSL2 上の Linux 版 SteamVR での実行(v0.3.0、2026-10-19)

Windows の WSL2(Ubuntu 24.04)に、steamcmd で Linux 版 SteamVR 2.17.10(ビルド 1789498647)と Steam Linux Runtime 3.0(sniper)を入れ、GitHub から取得した v0.3.0 をビルド・登録して実行しました。GPU は使えないため、Vulkan はソフトウェア実装(Mesa llvmpipe)です。

| 項目 | 結果 |
|---|---|
| ドライバのロード、`Active HMD set to vhmd.VHMD-HMD-0` | ✅ |
| `vhmd: HMD display: direct mode`、vsync クロック 90 Hz | ✅ |
| 3 点の登録、トラッカーのオンデマンド追加 | ✅ |
| 統合テスト(Background アプリで読める範囲) | ✅ 23 項目中 21 項目(列挙、オンデマンド追加、ポーズ誤差、遅延、不正パケット、1000 pps、終了処理)。残り 2 項目は Scene アプリの接続で、コンポジタが動かないため失敗 |
| vrcompositor が起動し、ダイレクトモードのドライバを認識 | ✅ `Direct mode: enabled`、`Headset is using driver direct mode`(v0.2.0 で出ていた `CannotDRMLeaseDisplay` は出ない) |
| vrcompositor のレンダラ初期化 | ❌ `physical device does not support timeline import`(llvmpipe はタイムラインセマフォの外部取り込みに非対応。実 GPU のドライバは対応) |

この環境特有の準備(実機の Linux では Steam クライアントが自動でやるので不要):
- Steam クライアント無しで動かしたため、Steam が提供する起動サービスを `SteamLinuxRuntime_sniper/pressure-vessel/bin/steam-runtime-launcher-service --alongside-steam` で代わりに立て、コンポジタを `VALVE_SKIP_RUNTIME_SAFETY=1` でホストのライブラリで起動した。
- Ubuntu 24.04 標準の Mesa 25.0 の llvmpipe は `VK_KHR_external_semaphore_fd` が無く、デバイス作成で失敗した。Mesa 26.2.5(kisak-mesa PPA)で解消。
- 新しい Mesa では Vulkan on D3D12(dzn)が先頭に来るが拡張が足りないため、`VK_DRIVER_FILES` で llvmpipe に限定した。

→ ドライバ側とダイレクトモードの受け入れまではソフトウェア Vulkan で確認できました。描画が流れることの確認には実 GPU が必要です(`tests/integration/linux_check.py` を実機で実行)。

### 実 GPU の Linux 版 SteamVR での実行(v0.3.0、2026-10-19、クラウド GPU)

Linode の GPU インスタンス(NVIDIA RTX 4000 Ada 20 GB、4 コア、Ubuntu 26.04.1、カーネル 7.0)に NVIDIA ドライバ 595.99(Vulkan 1.4)を入れ、GitHub の main(d81d6d3)を gcc 15.2 / CMake 4.2 でビルド(警告 0、ctest 合格)。steamcmd で SteamVR 2.17.10(ビルド 1789498647)と sniper を入れ、Steam クライアントもディスプレイも無い状態で Xvfb を使って `tests/integration/linux_check.py` を実行しました。

| 項目 | 結果 |
|---|---|
| ドライバのロード、UDP 応答、ダイレクトモード、vsync | ✅ |
| コンポジタがスワップテクスチャを作成 | ✅ 2 セット、片目 2160×2400、`VK_FORMAT_R8G8B8A8_SRGB`(43) |
| present レート | ✅ 90 回/秒(vsync と一致) |
| `vrcompositor.txt` に DRM リース等のエラー | ✅ なし |
| プレビュー(共有メモリ) | ✅ 1920×1067、SteamVR の既定の空間(地平線とグリッド)が左右の目に描画されている |
| 統合テスト `run_integration.py` | ✅ 28/28(列挙、オンデマンド追加、ポーズ、遅延、入力、スケルトン、1000 pps)。全体を 2 回連続で実行し、どちらも合格 |
| SteamVR の終了とドライバの終了処理 | ⚠️ 確認できず(下記)。テストは SKIP として扱う |

途中で見つかり対処したこと:
- コンポジタが起動しない: Steam クライアントが提供するライブラリが無かった(`libSDL2-2.0.so.0`、`libpulse.so.0`、`libpipewire-0.3.so.0`)。パッケージを追加して解消。`~/.steam/sdk64/steamclient.so` も steamcmd のものへリンク。
- 起動サービスを `dbus-run-session` 内で立てると vrserver から見えない。ユーザーのセッションバス(`loginctl enable-linger`)を使う。
- SteamVR Home(`steamvr_environments`)が画面の無い環境で起動・失敗を繰り返し、Scene アプリの接続が `AnotherAppLaunching` で失敗した(1 回)。`enableHomeApp: false` で解消。
- スケルトン確認で `InputError_NoData` が出た回が 1 回あり、詳細表示の読み出しで例外になってスクリプトごと止まっていた。読み出しを例外安全にした(その後の 2 回は全項目合格)。
- vrserver は SIGTERM / SIGINT を無視し、vrmonitor からの指示でしか終了しない。Xvfb 上では vrmonitor が起動直後に SIGFPE で落ちるため、正常終了させる手段が無かった。ドライバから `VREvent_DriverRequestedQuit` を送る方法も試したが vrserver は終了しなかった(この変更は取り込んでいない)。`stop_steamvr` は vrmonitor が居ない Linux では SteamVR を強制終了し、`ForcedShutdown` を返すようにした。

→ Linux のダイレクトモードで実 GPU のコンポジタから映像が流れることを確認しました。続けて同じ環境で下記を確認しました。

### デスクトップ相当の環境(v0.3.0、2026-10-19、同じクラウド GPU)

Xvfb をやめ、NVIDIA の実 X サーバー(仮想モニター、60 Hz)を立てて確認しました。

| 項目 | 結果 |
|---|---|
| vrmonitor の起動 | ✅(Xvfb では起動直後に SIGFPE で落ちていた) |
| SteamVR の正常終了とドライバの終了処理 | ✅ `vhmd: shutting down`、2.3 秒 |
| `linux_check.py`(Home 無効) | ✅ 14/14 |
| Steam クライアントあり + SteamVR Home 有効 | ✅ 14/14、統合テスト 28/28、正常終了。Home(`steamtours`)も起動 |

Steam クライアントを入れると、SteamVR の初回セットアップ(`vrsetup.sh`)が `vrcompositor-launcher` に `CAP_SYS_NICE` を付けるため管理者パスワードを求めて止まった。画面の無い環境では入力できないため `setcap` で同じ設定をした。

### Proton 経由の VRChat(v0.3.0、2026-10-19、同じクラウド GPU)

Steam クライアントで VRChat(438100)を入れると、Proton Experimental が自動で選ばれた。

| 項目 | 結果 |
|---|---|
| VRChat が Proton で起動し、SteamVR に Scene アプリとして接続 | ✅ |
| 仮想コントローラーへのバインディング | ✅ VRChat 同梱の `bindings_oculus_touch.json` が `vhmd_controller` に適用 |
| 仮想 HMD への描画 | ✅ プレビューに VRChat のログイン画面(左右の目) |
| 手の追従 | ✅ 右コントローラーの向きを ±15° 振ると、VRChat 内の手の向きと位置が変わる |
| ログイン後の操作(アバターの追従、フルボディ較正、指の曲がり) | 未確認(VRChat アカウントでのログインが必要なため実施していない) |

### Proxmox VM の GTX 1050(v0.3.0、2026-10-19)

Proxmox VE 9.0 上に VM(q35 / OVMF、6 コア、8 GB)を作り、GTX 1050(2 GB、Pascal)をパススルー。Ubuntu 26.04、NVIDIA ドライバ 580.178(Pascal に対応する最後の系列)、実 X サーバー + Steam クライアント。

| 項目 | 結果 |
|---|---|
| ビルド、単体テスト | ✅ 警告 0、ctest 合格 |
| `linux_check.py`(Steam クライアント、Home 有効) | ⚠️ 14/14 になったが、下記のとおり判定後に描画が止まっていた |
| 長時間の描画 | ❌ 起動から 5〜60 秒後に vrcompositor が `NVRM: Xid 13: 3D WIDTH ZT Violation` → `VK_ERROR_DEVICE_LOST` で停止(present 0、Scene アプリは 10 fps に低下)。SteamVR 自体は落ちない |
| 切り分け | プレビュー無効でも、SteamVR Home 無効でも、テストのクライアントを一切つながなくても、VM を再起動した直後でも再現。例外は vrcompositor 自身の GPU チャネルで発生。共有イメージの作成パラメータは ALVR と同じ。RTX 4000 Ada では起きない |
| VRChat | ❌ Easy Anti-Cheat が `Cannot run under Virtual Machine` で起動を拒否(VM の検出を隠す設定はアンチチート回避になるため行っていない) |

→ GTX 1050 / ドライバ 580 と SteamVR 2.17.10 の組み合わせで起きる問題と判断(GTX 1050 は SteamVR の最小要件 GTX 970 / 1060 未満)。ドライバ側の原因は見つかっていない。これを受けて `linux_check.py` に、終了前にもう一度 present を数える「compositor still presenting at the end」を追加し、この現象を FAIL として検出できることを確認した。

この VM の検証中に、ドライバの不具合を 1 件見つけて修正した:
- SteamVR が異常終了したあと、次の SteamVR でドライバが `bind(127.0.0.1:39570) failed` になった。ドライバの UDP ソケットが close-on-exec 無しで作られていたため、vrserver が起動した子プロセス(`steam-runtime-launch-client`)に引き継がれ、vrserver が終わってもポートを持ち続けていた。Linux は `SOCK_CLOEXEC`、Windows は `WSA_FLAG_NO_HANDLE_INHERIT` で作るよう修正し、共有イメージの fd も close-on-exec にした。単体テストに「ソケットが継承されない」確認を追加。

### 30 分の連続稼働(`tests/integration/soak_test.py`、v0.3.0、2026-10-19)

HMD・両手・トラッカー 3 台に 90 Hz でゆっくりした揺れを送りながら、Scene アプリとして毎フレーム `waitGetPoses` し、30 秒ごとに記録。

| 環境 | 結果 |
|---|---|
| Windows 11(RX 7900 XTX、200 Hz モニター、ウィンドウモード) | 30 分・60 サンプル。SteamVR 稼働継続、拒否パケット 0、デバイスの更新間隔 最大 12 ms、右手の姿勢誤差 最大 1.48 cm、vrserver のメモリ 323 MB で増加なし、vhmd の警告 0。Scene アプリのフレームレートは 平均 64 fps(下記) |
| Linux、RTX 4000 Ada(クラウド) | 1.5 分の試運転で全項目合格(90 fps、present 90/s、誤差 0.14 cm、メモリ 249 MB で一定)。30 分の本番はインスタンス停止のため途中で終了 |
| Linux、GTX 1050(Proxmox VM) | ❌ 上記のコンポジタ停止により不合格 |

Windows のウィンドウモードのフレームレート: Scene アプリは HMD の設定(90 Hz)より低い約 64〜67 fps で回る。テクスチャを毎フレーム送る D3D11 のテストアプリでも 66.4 fps(送らない場合 66.7 fps)、HMD の周波数を 100 / 200 Hz にしても変わらず、コンポジタの描画時間は 1 ms 未満。原因は特定できていない。姿勢・入力・遅延には影響しない。`soak_test.py` の基準は、この結果を見たうえでウィンドウモードのみ 60 fps とした(ダイレクトモードは 80 fps)。


### VRChat にログインした状態での確認(v0.3.0 + 開発版、2026-10-19、クラウド GPU)

新しい Linode(RTX 4000 Ada、Ubuntu 26.04、NVIDIA 実 X サーバー + Steam クライアント)で、Proton Experimental の VRChat(2026.3.3p2)を VR モードで起動。VRChat は Steam アカウント連携で自動ログインし、自分だけのプライベートインスタンス(ホームワールド)に入った。操作はすべて仮想デバイスへの UDP 送信で行い、映像はプレビュー(共有メモリ)で確認。

| 項目 | 結果 |
|---|---|
| アバターの頭・手の追従 | ✅ HMD の向き、両手の位置と向きが送った値どおりに反映 |
| 指(スケルトン) | ✅ 開く / トリガー + グリップからの自動生成(握る)/ `skeleton_curl` の指ごとの値(ピースサイン、L 字)がそのまま表示 |
| 移動・ボタン | ✅ 左スティックで歩く、左手 X でマイクのミュート切り替え |
| メニュー | ✅ 骨格が Partial のとき、左手 Y でクイックメニューが開く。Full のときは開かない(下記) |
| フルボディ較正(腰・両足トラッカー) | ✅ コントローラーモード(Windows)・ハンドモード(Linux)の両方で完了。ハンドモードでは、クイックメニューの「キャリブレーション」を手のひらのレーザー + トリガーで押し、T ポーズで両手のトリガーを押して確定。その後、左足 / 右足のトラッカーを前に上げると、アバターの左脚 / 右脚がそれぞれ上がることを確認 |
| コントローラーモードでのメニュー操作 | ✅ Windows の VRChat で確認。レーザーをメニューの**正面から**当てるとボタンがハイライトされ、トリガーでクリックできる(「ソーシャル」タイル → ソーシャルのページが開く)。当初「クリックできない」と記録したのは、自動操作でメニューに急な角度から当てていたため(下記) |

分かったこと:
- **VRChat は骨格の精度で操作方式を切り替える。** Full(送ったボーンをそのまま反映)だとハンドトラッキング扱いになり、メニューはジェスチャー操作(ボタンは無視)、ポインターは手のひらから出る。Partial だとコントローラー扱いで、Oculus Touch と同じボタン割り当て(Y / B でメニュー)が使える。どちらでも VRChat の入力処理は `_OPENXR_GENERIC`(他社製品のなりすましはしていないため)。
- これを受けて、**コントローラー ⇄ ハンドトラッキングを SteamVR を止めずに切り替える機能**(`config` の `hand_mode`)を実装した。手ごとにコントローラー(Partial)とハンド(`VHMD-HAND-L/R`、Full)の 2 デバイスを持ち、片方だけを接続中にする。既定の `skeletonLevel` は `partial` に変更。
- VRChat を起動したまま controllers → hands → controllers と切り替え、Y ボタンでメニューが開く → 開かない → 開くことを確認。統合テスト(`skeleton_check.py`)でも、手の役割がハンド側へ移り、送ったボーンが誤差 0.000° で届き、元に戻ることを確認(Linux 30/30、Windows 31/31)。
- コントローラーモードのクリックについて(2026-10-19、Windows 11 の VRChat):
  - 当初は「クリックできない」と判断したが、原因は検証の操作だった。メニュー(左手)に対して右手を斜め下・横から向けており、レーザーの見た目はボタンに届いていても、VRChat の当たり判定では面に当たっていなかった(角度が浅い)。
  - 右手をメニューの正面(体側、メニュー中心と同じ高さ)に置き、面にほぼ垂直に当てると、ボタンがハイライトされ、トリガーでクリックできた(Linux の結果も同じ原因と考えられる)。
  - 切り分けの途中で確認したこと: SteamVR 側は正常(VRChat のアクションマニフェストを読み込んだテスト用アプリで、トリガー → `Menu/Confirm`・`Global/Interact` がオン)。ウィンドウのフォーカス、メニューの開き直し、左右の手の入れ替えでは変化なし。コントローラー種別を Index と申告する診断ビルド(コミットしていない)でも VRChat の扱いは汎用のままだった。
  - 位置関係の目安(SteamVR の座標、-z が前方。実際に姿勢を読み出して確認): 頭 (0, 1.60, 0)、メニューを持つ左手 (-0.10, 1.32, -0.35) で、メニューの中心はその約 13 cm 上。指す右手は体の近く (0.08, 1.30, -0.05) に置き、メニューまで約 0.35 m 離す。この配置で、右手の向き yaw -16°, pitch +50° のときレーザーが「キャリブレーション」に乗る(ボタンの縦幅は向きにして約 2°)。最初の失敗は、右手をメニューの 15〜25 cm 横・下に置き、面に対して 60° 以上傾けて当てていたため。
  - この配置で Windows のコントローラーモードでもフルボディ較正を完了: 「キャリブレーション」をクリック → アバターの前にキャリブレーション用の表示 → T ポーズで両手のトリガー → 左足トラッカーを上げるとアバターの左脚が上がる。
  - 指先などの部品を持つ独自レンダーモデル(`{vhmd}vhmd_controller`、`tools/gen_rendermodel.py`)を追加した。クリック問題の原因ではなかったが、レーザーがモデルの先端から出るなど他アプリにも有用なため採用。
- 自動操作の注意: クライアントが `trigger_click` を一度でも明示的に送ると、それ以降ドライバは `trigger` の値からクリックを自動生成しない(明示指定を優先する仕様)。混在させると押したつもりのクリックが出ない。
- 操作上の注意: HMD がほとんど動かないと SteamVR が待機(activity level Standby)に入り、VRChat がボタン入力を受け付けなくなる。自動操作では HMD を数 cm 揺らし続ける必要がある。

### 負荷の削減(2026-10-19、Windows 11)

ドライバに自分の負荷を測る仕組みを追加し(`status` の `perf`)、無駄を 3 つ削った。

- 変化のない姿勢(速度 0)は毎回送らず、250 ms ごとに再送するだけにした(止まっているトラッカーや、送信が 90 Hz で更新ループが 120 Hz のときの重複)。
- ハンドモードの待機側(切断中のデバイス)は、姿勢の送信を 1 秒ごとにし、指の骨格の計算をしない。
- ボタンから作る指の骨格は、曲げの値が変わったときだけ計算し直す(変化がなければ 1 秒ごとに再送)。
- Linux のプレビューは既定で停止し、見るツールが頼んだときだけ動かす(以前は誰も見ていなくても 1 フレーム約 8 MB を毎秒 15 回コピーしていた)。

| 条件 | 更新 1 回の処理時間(前 → 後) | vrserver 全体の CPU(1 コア比、前 → 後) |
|---|---|---|
| 待機(クライアントなし) | 42 µs → 9〜10 µs | 4.8 % → 2.6〜3.6 % |
| 送信中(コントローラー + トラッカー 3) | 66 µs → 49〜52 µs | 5.9 % → 4.0〜4.8 % |
| 送信中(ハンドモード + トラッカー 3) | 78 µs → 49〜53 µs | 7.7 % → 4.0〜4.8 % |

vrserver 全体の値は SteamVR 本体や他のドライバの分を含み、測るたびに ±1.5 % ほど揺れる。ドライバ自身の受信スレッドは送信中に 1 コアの約 1 %(1 秒あたり約 630 パケットの JSON 解析)。統合テスト 31/31。

### Windows のダイレクトモードと FPS 制限(2026-10-19、Windows 11、RX 7900 XTX)

`display.mode = "direct"` で、SteamVR がデスクトップにウィンドウを出さず、ドライバが D3D11 の共有テクスチャで映像を受け取る方式を追加した(ALVR の Windows ドライバと同じ考え方、コードは流用していない)。プレビューウィンドウ(`previewWindow` / `preview_window` 操作)と、Linux と同じ共有メモリへの映像書き出し(名前付きマッピング `vhmd_preview`)を、必要なときだけ動かす。

- コンポジタが `Headset is using driver direct mode` を出し、毎秒 90 回 present(ウィンドウモードでは 66 fps で頭打ちだった)。統合テスト 31/31(ダイレクトモードで実行)。
- レンズ歪みなしの左右の目の映像がプレビューウィンドウと Python(`PreviewReader`)の両方で取れる。
- 起動直後に 1 回だけ `WaitForAcquire timed out` の警告(同期テクスチャを最初に取るまで)。以降は出ない。

負荷の比較(毎フレーム描画する D3D11 のテストアプリ + 頭と両手の姿勢送信。各条件で SteamVR を再起動し 15 秒計測。CPU は 1 コア比、GPU は Windows の GPU Engine カウンタの 3D 使用率):

| 条件 | アプリの fps | CPU: compositor + vrserver + アプリ | GPU(3D): compositor + vrserver + アプリ |
|---|---|---|---|
| ウィンドウモード 90 Hz(従来) | 66.6 | 32.1 + 5.1 + 1.6 = **38.8 %** | 計測不可 + 0 + 3.7 % |
| ダイレクト 90 Hz(ウィンドウなし) | 89.9 | 10.0 + 6.9 + 1.5 = **18.4 %** | 6.8 + 0 + 2.7 = 9.5 % |
| ダイレクト 90 Hz + プレビューウィンドウ | 89.9 | 10.7 + 8.0 + 2.0 = 20.7 % | 7.1 + 1.5 + 2.5 = 11.1 % |
| ダイレクト 90 Hz + Python へ 15 fps で映像 | 89.9 | 11.2 + 6.6 + 1.8 = 19.6 % | 6.8 + 0.6 + 2.6 = 10.0 % |
| ダイレクト 60 Hz | 59.9 | 7.2 + 7.8 + 1.1 = 16.1 % | 4.6 + 0 + 1.8 = 6.4 % |
| ダイレクト 30 Hz | 30.0 | 5.5 + 7.2 + 0.0 = **12.7 %** | 2.8 + 0 + 0.9 = **3.7 %** |
| ダイレクト 15 Hz | 15.0 | 3.1 + 6.0 + 0.3 = **9.4 %** | 1.3 + 0 + 0.6 = **1.9 %** |

- ウィンドウモードの compositor の GPU 使用率はカウンタに出なかった(計測不可)。dwm はアクセス権の都合で 0 と出るため表から省いた。
- vrserver は SteamVR 本体と他のドライバを含み、条件によらず 5〜8 % で揺れる。
- テストアプリは画面を塗りつぶすだけの軽いものなので、実際のアプリでは fps を下げた効果はもっと大きい。

## 2. 手動検証手順(要件 10.2)

GUI 操作が必要なため未実施の項目です。結果欄に記入してください。

### 準備

```bash
powershell -ExecutionPolicy Bypass -File tools/verify_env.ps1
powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1
```

1. PICO 等の実 HMD を接続していないことを確認(接続中だと vhmd の HMD がアクティブにならない可能性あり)。
2. SteamVR を起動。デスクトップに「VR ビュー」相当のウィンドウ(仮想ディスプレイ)が開く。
3. 別ターミナルで `python clients/python/examples/fullbody_sway.py` を実行し続ける。

| # | 手順 | 期待結果 | 結果 |
|---|---|---|---|
| M1 | SteamVR ステータスウィンドウを見る | HMD・コントローラ 2・トラッカー 8 のアイコンが緑(点灯) | 未検証 |
| M2 | SteamVR メニュー → 設定 → コントローラー → 「トラッカーの管理」 | VHMD-TRK-0〜7 が並び、腰・左足…のロールが既に入っている。変更でき、変更後の値が再起動後も残る(ドライバは既存ロールを上書きしない) | 未検証 |
| M3 | SteamVR メニュー → 「ヘッドセットのビュー」 | 視点が揺れ・前後に動く | 未検証 |
| M4 | `python clients/python/examples/gamepad.py --demo` を追加実行し、SteamVR の「コントローラーのバインディング」→ 任意アプリ → vhmd_controller を開く | 入力テスト画面でトリガー/グリップ/スティック/A/B が動く | 未検証 |
| M5 | VRChat を VR モードで起動(Steam ライブラリから「SteamVR モードで起動」) | 起動しホーム画面が表示される(画面はデスクトップのウィンドウに出る) | 未検証 |
| M6 | M5 の状態で `fullbody_sway.py` | アバターの頭・手が揺れに追従 | 未検証 |
| M7 | VRChat のフルボディトラッキング較正(メニュー → キャリブレーション、`fullbody_sway.py` を一時停止して T ポーズ姿勢を送る) | 腰・両足トラッカーでアバターの下半身が動く。膝・肘・胸の割り当ては VRChat 側の設定次第 | 未検証 |
| M8 | `gamepad.py --demo` 実行中に VRChat で手を見る | トリガー/グリップに合わせて指が曲がる(`skeleton` 自動生成)。`skeleton_curl` を直接送っても同様 | 未検証 |
| M9 | 30 分放置(`fullbody_sway.py` 送信継続) | SteamVR / VRChat が落ちない。`vrserver.txt` に `update thread jitter` 警告が多発しない | 未検証 |
| M10 | 視線: `VHMDClient().gaze(direction=(0.2,0,-1))` を送り、OpenXR 視線対応アプリで確認 | 視線が反映(対応アプリがあれば) | 未検証 |

#### VRChat 用の T ポーズ例

```python
from vhmd_client import VHMDClient
with VHMDClient() as c, c.batch():
    c.pose("hmd", (0, 1.65, 0))
    c.pose("left_hand", (-0.75, 1.45, 0)); c.pose("right_hand", (0.75, 1.45, 0))
    c.pose("waist", (0, 1.0, 0)); c.pose("left_foot", (-0.1, 0.08, 0)); c.pose("right_foot", (0.1, 0.08, 0))
```
(タイムアウトポリシー `hold` なので、送るのを止めてもその姿勢のまま保持されます)

### 注意

- VRChat のアカウント操作・ワールド移動などは行わないでください(この検証の範囲外)。
- 実 HMD を使う前に `tools/uninstall_driver.ps1` で登録解除するか、`hmd.enabled: false` にしてください。
