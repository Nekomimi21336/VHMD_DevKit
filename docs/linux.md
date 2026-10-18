# Linux 版 SteamVR での使い方

Linux ネイティブの SteamVR に `driver_vhmd.so` を読み込ませる手順です。Proton で動く Windows ゲーム(VRChat 等)も Linux 版 SteamVR に接続するので、このドライバが必要になります。送信側のプロトコル・Python クライアントは Windows と共通です。

## 状態(2026-10-18 時点)

| 項目 | 状態 |
|---|---|
| Linux ビルド (gcc 13.3, Ubuntu 24.04 / WSL2) | ✅ 警告 0(`-Wall -Wextra -Wpedantic`) |
| C++ 単体テスト 36 件 | ✅ すべて成功(UDP ループバック含む) |
| 公開シンボル | ✅ `HmdDriverFactory` のみ(リンカのバージョンスクリプト) |
| 依存ライブラリ | ✅ `libc`, `libm`, `ld-linux` のみ(libstdc++ / libgcc は静的リンク) |
| `tools/*.sh` | ✅ 偽の SteamVR 環境で install / uninstall / `--clean-settings` を確認 |
| 統合テストの Linux 対応 (`tests/integration/steamvr_util.py`) | ⚠ WSL 上で読み込みとプロセス検出のみ確認 |
| **実機の Linux 版 SteamVR での動作** | ❌ **未検証**(この開発 PC には Linux 版 SteamVR が無い) |

## 前提

- x86_64 Linux、Steam + SteamVR(Linux ネイティブ版)
- g++ 9 以上(または clang)、CMake 3.20 以上、Git、Python 3.10 以上
- Ubuntu / Debian 例: `sudo apt install build-essential cmake ninja-build git python3`

## ビルド

```bash
git clone --recurse-submodules <このリポジトリ>
cd VirtualHMD
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

出力: `build/driver/vhmd/bin/linux64/driver_vhmd.so`(+ `driver.vrdrivermanifest`, `resources/`)。

### glibc の注意

できあがった `.so` は**ビルドした環境の glibc 以上**を要求します(Ubuntu 24.04 でビルドすると glibc 2.38 以上)。`tools/verify_env.sh` の `driver_needs_glibc` で確認できます。**SteamVR を動かす PC 自身でビルドする**のが確実です。古いディストリにも配る場合は、古い環境(または Steam Runtime SDK のコンテナ)でビルドしてください。

## 登録・解除

```bash
tools/verify_env.sh            # 環境の確認(読み取りのみ)
tools/install_driver.sh        # vrpathreg.sh adddriver build/driver/vhmd
tools/uninstall_driver.sh --clean-settings   # 登録解除 + SteamVR 設定から vhmd の項目を削除(バックアップ付き)
```

- `vrpathreg.sh` は `<SteamVR>/bin/vrpathreg.sh`。SteamVR の場所は `~/.config/openvr/openvrpaths.vrpath` の `runtime`、無ければ `~/.steam/steam`, `~/.local/share/Steam`, Flatpak の順に探します。
- SteamVR 本体フォルダへのコピーはしません。
- **ビルドし直すときは SteamVR を終了してください。**

## 使う

```bash
python3 clients/python/examples/fullbody_sway.py
```

ログ: `~/.steam/steam/logs/vrserver.txt`(場所は `openvrpaths.vrpath` の `log`)。`vhmd:` で始まる行がこのドライバのものです。

## 最大のリスク: 仮想ディスプレイ

HMD は `IVRDisplayComponent` で「デスクトップ上のウィンドウ」として表示する方式(Windows で動作確認済み、OpenVR サンプル `simplehmd` と同じ)です。**Linux 版 SteamVR のコンポジタがこの方式で起動するかは不明**です。起動しない場合の兆候と対処:

| 兆候 | 対処 |
|---|---|
| SteamVR が「ヘッドセットが見つからない / コンポジタが起動しない」 | まず `hmd.enabled: false`(実 HMD と併用するトラッカー/コントローラ専用モード)で他の機能だけ使う |
| 実 HMD が無い PC で HMD も仮想にしたい | `IVRVirtualDisplay` を実装する(`HmdDevice::GetComponent` で返すコンポーネントを差し替え。`docs/architecture.md` の拡張ポイント参照) |
| `vrcompositor` のログに Vulkan / ディスプレイのエラー | `vhmd_display` の `windowWidth/Height` を画面内に収める、Wayland なら X11 セッションでも試す |

## 実機での確認チェックリスト

結果を記入して `docs/verification.md` に反映してください。

| # | 手順 | 期待結果 | 結果 |
|---|---|---|---|
| L1 | `tools/verify_env.sh` | SteamVR のパス、glibc ≥ `driver_needs_glibc` | 未検証 |
| L2 | `tools/install_driver.sh` → SteamVR 起動 | `vrserver.txt` に `Loaded server driver vhmd` と `vhmd: listening on udp://127.0.0.1:39570` | 未検証 |
| L3 | 同ログ | `Active HMD set to vhmd.VHMD-HMD-0`(実 HMD 未接続時)。コンポジタが起動するか(上記リスク) | 未検証 |
| L4 | `pip install openvr` → `python3 tests/integration/run_integration.py` | Windows と同じ 27 項目。`--stop-relaunchers` は自動起動オーバーレイを終了させるので必要時のみ | 未検証 |
| L5 | `python3 clients/python/examples/fullbody_sway.py` + SteamVR の「ヘッドセットのビュー」 | 視点とデバイスが動く | 未検証 |
| L6 | Proton で VRChat を VR モード起動 | HMD・手・トラッカーが反映される | 未検証 |
| L7 | `hmd.enabled: false` + 実 HMD | 実 HMD と並んでコントローラ/トラッカーが追加される | 未検証 |
| L8 | SteamVR を終了 | `vhmd: shutting down` が出て vrserver が終了する(ハングしない) | 未検証 |

## Linux 対応で入れた変更

- `driver/driver_exports.map` + `-Wl,--version-script`: `HmdDriverFactory` 以外を非公開(vrserver は複数ドライバを同じプロセスに読み込むためシンボル衝突を防ぐ)。
- `-static-libstdc++ -static-libgcc`: SteamVR 実行環境の libstdc++ が古くても読み込めるように。`-Wl,--no-undefined` で未解決シンボルをビルド時に検出。
- gcc 固有の警告 4 件を修正(初期化、OSC バンドルの長さ読み取りの戻り値確認、一時オブジェクトへの参照)。
- `.gitattributes`: `*.sh` を LF 固定(Windows の `core.autocrlf` で壊れないように)。
- `tests/integration/steamvr_util.py`: `/proc` によるプロセス検出、シグナルでの停止、`vrstartup.sh` 起動、`~/.steam` / XDG パス。OpenVR クライアントの検出は `vrclient.so` を読み込んだプロセスで行い、Steam 本体・`steamapps` 配下(Proton ゲーム含む)は対象外。
