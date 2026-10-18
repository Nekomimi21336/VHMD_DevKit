**日本語** | [English](en/linux.md) | [简体中文](zh-CN/linux.md) | [한국어](ko/linux.md) — [README に戻る](../README.md)

# Linux 版 SteamVR での使い方

Linux ネイティブの SteamVR に `driver_vhmd.so` を読み込ませる手順です。Proton で動く Windows ゲーム(VRChat など)も Linux 版 SteamVR に接続するので、このドライバが必要になります。送信側のプロトコルと Python クライアントは Windows と共通です。

## 仕組み: Linux では「ダイレクトモード」で動く

Linux 版 SteamVR のコンポジタは、Windows のように「デスクトップ上のウィンドウに描く」ことができません。ダイレクトモード(DRM リース)で取れる表示先が無いと、`VRInitError_Compositor_CannotDRMLeaseDisplay` で起動を止めます。SteamVR 同梱の null ドライバでも同じで、Valve も既知の問題として扱っています([SteamVR-for-Linux#889](https://github.com/ValveSoftware/SteamVR-for-Linux/issues/889))。

そこで v0.3.0 から、Linux の仮想 HMD は **`IVRDriverDirectModeComponent`** を実装しています。

```
アプリ ──▶ vrcompositor ──(共有 Vulkan イメージ / dma-buf)──▶ driver_vhmd.so
                                                              ├─ vsync を自分で通知(既定 90 Hz)
                                                              └─ 最新フレームを /dev/shm/vhmd_preview へ(任意)
```

- 実ディスプレイも DRM リースも不要です。左右の目の画像はドライバが確保した共有 Vulkan イメージに描かれ、`Present` で渡されます(`IVRIPCResourceManagerClient` の Linux 専用 API)。
- ALVR の Linux ドライバ(2026-10 の master)と同じ方式です。コードは流用せず、OpenVR ヘッダに沿って実装しました。
- Windows は従来どおりデスクトップのウィンドウ方式です(`display.mode`: `auto` / `window` / `direct`)。

## 状態(2026-10-19 時点)

| 項目 | 状態 |
|---|---|
| Linux ビルド(gcc 13.3、Ubuntu 24.04 / WSL2) | ✅ 警告 0(`-Wall -Wextra -Wpedantic`) |
| C++ 単体テスト 37 件 | ✅ すべて成功(ASan / TSan でも問題なし) |
| フレームキャプチャのセルフテスト(`tests/linux/capture_selftest.cpp`) | ✅ 実際の Vulkan(Mesa llvmpipe)で fd インポート → 縮小 → 共有メモリ → 画素値まで確認 |
| 公開シンボル / 依存ライブラリ | ✅ `HmdDriverFactory` のみ / `libc`、`libm`、`ld-linux` のみ(libvulkan は実行時に `dlopen`) |
| v0.2.0 を Linux 版 SteamVR 2.17.10(Arch、KDE Wayland)で実行 | ✅ ドライバのロード、HMD 認識、コントローラのリマップ / ❌ コンポジタが `CannotDRMLeaseDisplay` で起動せず(null ドライバも同様)→ v0.3.0 のダイレクトモードで対応 |
| v0.3.0 を WSL2 上の Linux 版 SteamVR 2.17.10(ソフトウェア Vulkan)で実行 | ✅ ドライバのロード、ダイレクトモード、統合テスト 21/23、コンポジタが `Headset is using driver direct mode` まで到達 / ❌ llvmpipe がタイムラインセマフォの外部取り込みに非対応でレンダラ初期化が失敗([verification.md](verification.md)) |
| v0.3.0 を実 GPU の Linux 版 SteamVR 2.17.10 で実行(クラウド GPU: NVIDIA RTX 4000 Ada、ドライバ 595、Ubuntu 26.04、gcc 15.2 でビルド、画面なし + Xvfb) | ✅ `linux_check.py` 全項目合格: コンポジタがスワップテクスチャ(片目 2160×2400)を作り **90 fps で present**、`CannotDRMLeaseDisplay` なし、プレビューに SteamVR の映像、統合テスト 28/28(スケルトン・入力を含む)。2 回連続で合格 |
| デスクトップ相当の環境(NVIDIA の実 X サーバー + Steam クライアント + SteamVR Home 有効) | ✅ `linux_check.py` 14/14 合格。vrmonitor が動き、SteamVR の正常終了とドライバの終了処理(`vhmd: shutting down`、約 2 秒)まで確認。クラウド GPU(RTX 4000 Ada)で確認。Proxmox VM の GTX 1050(2 GB、Pascal、ドライバ 580)でも 14/14 になったが、**起動から 5〜60 秒後に vrcompositor が GPU 例外(`Xid 13: 3D WIDTH ZT Violation` → `VK_ERROR_DEVICE_LOST`)で描画を止める**。クライアント・プレビュー・SteamVR Home をすべて無しにしても再現し、ドライバ側の原因は見つかっていない(GTX 1050 は SteamVR の最小要件 GTX 970 / 1060 未満) |
| Proton 経由の VRChat(Proton Experimental) | ✅ 起動し、仮想 HMD に VR 映像(ログイン画面)を左右とも描画。仮想コントローラーに VRChat の Oculus Touch 用バインディングが適用され、VRChat 内の手が送った姿勢に追従。ログイン後も、アバターの頭・手・指の追従、移動、ボタン、メニュー、コントローラー ⇄ ハンドトラッキングの切り替え(VRChat を止めずに)を確認。フルボディ較正もハンドモードで完了し、足のトラッカーでアバターの脚が動くことを確認。コントローラーモードでも、レーザーをメニューの正面から当てればクリックできる(Windows で確認)。**仮想マシン内では VRChat の Easy Anti-Cheat が `Cannot run under Virtual Machine` で起動を拒否します**(クラウドの KVM では起動した。VM の検出を隠す設定はアンチチート回避になるため行っていません) |

## 前提

- x86_64 Linux、Steam + SteamVR(Linux ネイティブ版)
- g++ 9 以上(または clang)、CMake 3.20 以上、Git、Python 3.10 以上
- Vulkan ドライバ(SteamVR が動く環境なら入っています)。Vulkan のヘッダ(`vulkan-headers` / `libvulkan-dev`)は無ければ configure 時に自動取得します
- Ubuntu / Debian 例: `sudo apt install build-essential cmake ninja-build git python3`
- Arch 例: `sudo pacman -S base-devel cmake ninja git python vulkan-headers`

## ビルド

```bash
git clone --recurse-submodules https://github.com/Nekomimi21336/VHMD_DevKit.git
cd VHMD_DevKit
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

出力: `build/driver/vhmd/bin/linux64/driver_vhmd.so`(+ `driver.vrdrivermanifest`、`resources/`)。

### glibc の注意

できあがった `.so` は、**ビルドした環境の glibc 以上**を要求します(Ubuntu 24.04 でビルドすると glibc 2.38 以上)。`tools/verify_env.sh` の `driver_needs_glibc` で確認できます。**SteamVR を動かす PC 自身でビルドする**のが確実です。

## 登録・解除

```bash
tools/verify_env.sh            # 環境の確認(読み取りのみ)
tools/install_driver.sh        # vrpathreg.sh adddriver build/driver/vhmd
tools/uninstall_driver.sh --clean-settings   # 登録解除 + SteamVR 設定から vhmd の項目を削除(バックアップ付き)
```

- `vrpathreg.sh` は `<SteamVR>/bin/vrpathreg.sh` です。SteamVR の場所は `~/.config/openvr/openvrpaths.vrpath` の `runtime` から、無ければ `~/.steam/steam`、`~/.local/share/Steam`、Flatpak の順に探します。
- SteamVR 本体のフォルダへのコピーはしません。
- **ビルドし直すときは SteamVR を終了してください。**

## 使う

```bash
python3 clients/python/examples/fullbody_sway.py --points 6
```

SteamVR が仮想 HMD に描いている映像を見るには:

```bash
python3 clients/python/examples/preview_viewer.py                 # ウィンドウで表示(標準ライブラリのみ)
python3 clients/python/examples/preview_viewer.py --snapshot eyes.png
```

自作ツールから読むときは `vhmd_client.preview.PreviewReader` を使います(形式: `driver/src/util/preview_shm.h`。左右の目を横に並べた RGBA8、sRGB)。

| 設定(`vhmd_config.json` の `preview`) | 既定 | 内容 |
|---|---|---|
| `enabled` | `true` | 共有メモリへの書き出し |
| `path` | `/dev/shm/vhmd_preview` | 書き出し先 |
| `fps` | `15` | 書き出しの上限レート(コンポジタの描画レートとは無関係) |
| `maxEyeWidth` | `960` | 片目あたりの最大幅(縮小して書き出す) |
| `vulkanDevice` | `-1` | 使う GPU の番号。`-1` は最初の GPU。**SteamVR と同じ GPU を選ぶ必要があります**(複数 GPU の PC で映像が来ない場合に変更) |

プレビューの処理で失敗しても無効になるだけで、SteamVR への描画には影響しません。

ログ: `~/.steam/steam/logs/vrserver.txt`(場所は `openvrpaths.vrpath` の `log`)。`vhmd:` で始まる行がこのドライバのものです。`config list` の応答の `display` に、モード、present 数、vsync 数、プレビューの状態が入ります。

## 実機での確認(自動)

```bash
pip install openvr
python3 tests/integration/linux_check.py
```

SteamVR の起動から終了までを自動で行い、`tests/integration/results/linux_check_<日時>/` にレポート(`report.md`、`report.json`)、プレビューのスナップショット(`eyes.png`)、ログの抜粋を出力します。**このフォルダを送ってもらえれば状況が分かります。** 確認する内容:

| # | 確認 | 期待 |
|---|---|---|
| 1 | 環境(ディストリ、セッション種別、GPU、Vulkan、SteamVR のビルドとブランチ) | 情報 |
| 2 | ドライバのビルド・登録 | ✅ |
| 3 | SteamVR がドライバを読み込み、UDP に応答 | ✅ |
| 4 | HMD がダイレクトモード、vsync が回っている | ✅ |
| 5 | コンポジタがスワップテクスチャを作り、毎秒 30 回以上 present | ✅ これが今回の本題 |
| 6 | `vrcompositor.txt` に `CannotDRMLeaseDisplay` などが無い | ✅ |
| 7 | プレビューに進んでいくフレームがあり、`eyes.png` を保存 | ✅ |
| 8 | 通常の統合テスト(`run_integration.py`: 列挙、ポーズ、入力、スケルトン、負荷) | ✅ |
| 9 | SteamVR 終了時にドライバの終了処理が走る | ✅ |
| 10 | 終了前(既定で 60 秒待ってから)もコンポジタが present し続けている(`--hold` で秒数を変更) | ✅ |

`--skip-integration` で 8 を省略できます(表示経路だけ確認したいとき)。

## うまくいかないとき

| 症状 | 確認すること |
|---|---|
| 5 が 0(present が来ない) | `vrcompositor.txt` の末尾。SteamVR のバージョン(ALVR はベータ版で動作確認していた可能性があり、安定版 2.17.10 で同じように動くかは未確認)。`steamvr.vrsettings` に、以前 ALVR などで入れた起動オプションや `forcedDriver` が残っていないか |
| present は来るがプレビューが真っ黒 / 来ない | `report.json` の `display.preview.state`。複数 GPU の場合は `preview.vulkanDevice` |
| VRChat が起動しない | 1〜7 がすべて ✅ になってから試してください。Proton 側のログ(`PROTON_LOG=1`) |
| 実 HMD と併用したい | `"hmd": {"enabled": false}`(コントローラ / トラッカーだけ追加) |
| 画面の無いマシン(クラウド GPU など)で SteamVR Home が起動と失敗を繰り返し、Scene アプリが `AnotherAppLaunching` になる | `steamvr.vrsettings` の `steamvr` に `"enableHomeApp": false` |
| 画面の無いマシンで「SteamVR shutdown」が SKIP | vrserver は vrmonitor(SteamVR のウィンドウ)からの指示でしか終了せず、SIGTERM も無視します。Xvfb では vrmonitor 自体が起動時に落ちるため、テストは SteamVR を強制終了します(ドライバの終了処理は通りません)。デスクトップでは通常どおり終了します |
| Steam クライアントで初めて SteamVR を起動すると管理者パスワードを求められ、止まる | SteamVR のセットアップ(`vrsetup.sh`)がコンポジタに `CAP_SYS_NICE` を付けようとしています。パスワードを入力するか、`sudo setcap CAP_SYS_NICE=eip <SteamVR>/bin/linux64/vrcompositor-launcher` |
| VM 内で VRChat が `Cannot run under Virtual Machine` | VRChat の Easy Anti-Cheat の仕様です。実機の Linux で実行してください |
| 最初は合格するが「compositor still presenting at the end」が FAIL、`dmesg` に `NVRM: Xid 13` | GPU の例外でコンポジタが止まっています。GTX 1050(Pascal、ドライバ 580)で再現。SteamVR の最小要件(GTX 970 / 1060)以上の GPU で試してください |
| VM / WSL など GPU の無い環境 | ソフトウェア Vulkan(llvmpipe)ではコンポジタのレンダラ初期化が `does not support timeline import` で止まります。実 GPU が必要です |

## 画面の無いマシン(クラウド GPU など)で動かす

Steam クライアントもディスプレイも無い Linux で検証したときの手順です。普通のデスクトップでは不要です。

1. GPU ドライバ(NVIDIA なら `nvidia-driver-5xx`)を入れ、`vulkaninfo --summary` に GPU が出ることを確認
2. `steamcmd` で SteamVR(250820、要ログイン)と Steam Linux Runtime 3.0 sniper(1628350)を入れる。`~/.steam/steam` → Steam フォルダのシンボリックリンク、`~/.steam/sdk64/steamclient.so` → `steamcmd/linux64/steamclient.so`
3. SteamVR がホストのライブラリで動くための追加パッケージ(Ubuntu): `libsdl2-2.0-0 libpulse0 libpipewire-0.3-0t64`
4. Ubuntu 24.04 以降は非特権ユーザー名前空間が制限されているため `sysctl kernel.apparmor_restrict_unprivileged_userns=0`(pressure-vessel が使用)
5. `Xvfb :1` を起動し `DISPLAY=:1`。ユーザーのセッションバス(`loginctl enable-linger <user>` で `/run/user/<uid>/bus`)を `DBUS_SESSION_BUS_ADDRESS` に設定
6. Steam クライアントの代わりに起動サービスを立てる: `VALVE_SKIP_RUNTIME_SAFETY=1 SteamLinuxRuntime_sniper/pressure-vessel/bin/steam-runtime-launcher-service --alongside-steam --session &`
7. `steamvr.vrsettings` で `"enableHomeApp": false`、`tools/install_driver.sh`、`python3 tests/integration/linux_check.py`

## Linux 対応で入れた変更

- **v0.3.0**: `driver/src/linux/`(`direct_mode.*`: ダイレクトモードのコンポーネントと vsync クロック、`frame_capture.*`: 共有イメージのインポートと縮小コピー、`vk_loader.*`: libvulkan の動的ロード)、`util/preview_shm.h`、Python の `preview.py` / `preview_viewer.py`、`tests/linux/capture_selftest.cpp`、`tests/integration/linux_check.py`。
- `driver/driver_exports.map` + `-Wl,--version-script`: `HmdDriverFactory` 以外を非公開にします(vrserver は複数のドライバを同じプロセスに読み込むため)。
- `-static-libstdc++ -static-libgcc`: SteamVR の実行環境の libstdc++ が古くても読み込めるようにします。
- `.gitattributes`: `*.sh` を LF に固定します。
- `tests/integration/steamvr_util.py`: `/proc` でのプロセス検出、シグナルでの停止、`vrstartup.sh` での起動、`~/.steam` / XDG のパス。
