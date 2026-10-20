<div align="center">

# VHMD_DevKit

**開発用 仮想 HMD / コントローラ / フルトラッカー SteamVR ドライバ**

実機が無くても、外部プロセスから UDP/JSON で送ったポーズ・入力を SteamVR に「実機があるかのように」見せます。

**日本語** | [English](README.en.md) | [简体中文](README.zh-CN.md) | [한국어](README.ko.md)

</div>

---

## 目次

- [特長](#特長)
- [3 点から始めて、必要な分だけトラッカーを足す](#3-点から始めて必要な分だけトラッカーを足す)
- [はじめかた(ビルド済みバイナリ)](#はじめかたビルド済みバイナリ)
- [ソースからビルド](#ソースからビルド)
- [使い方(Python)](#使い方python)
- [ウィンドウを出さずに使う / FPS を下げる(Windows)](#ウィンドウを出さずに使う--fps-を下げるwindows)
- [設定](#設定)
- [実機 HMD と併用するとき](#実機-hmd-と併用するとき)
- [テスト](#テスト)
- [ドキュメント](#ドキュメント)
- [リポジトリ構成](#リポジトリ構成)
- [動作確認環境](#動作確認環境)
- [ライセンス](#ライセンス)

## 特長

- **3 点 VR が基本**: HMD + 左右コントローラは SteamVR 起動時から常に存在
- **トラッカーはオンデマンド**: 初めて送った時点で SteamVR に追加され、そのポーズから同期して始まる(最大 16 台)
- **シンプルな外部 API**: UDP + JSON(`127.0.0.1:39570`)。1 データグラムで全デバイスをまとめて更新可能。任意で VMT 互換 OSC(Joint / Follow / スケルトン / ルーム行列 / 出力メッセージを含む VMT 0.15 の API)
- **コントローラ入力一式**: トリガー / グリップ(force 付き)/ スティック / トラックパッド / サムレスト / A・B・X・Y / システム / メニュー、振動の返送、指スケルトン(曲げ値 or ボーン直接指定)。トラッカーにもボタン・トリガー・トラックパッド
- **デバイスの状態**: HMD の装着センサー(SteamVR の待機と OpenXR の `XR_EXT_user_presence` に反映)、バッテリー残量と充電中の表示、姿勢の加速度とサンプルの遅れ(`poseTimeOffset`)
- **コントローラとハンドトラッキングの切り替え**: SteamVR を止めずに `hand_mode` で切り替え(VRChat ではボタン操作 ⇄ ハンドトラッキング操作)。送る ID と内容はそのまま
- **SteamVR 上で見分けやすい表示**: トラッカーのモデルとステータスアイコンに部位(`L FOOT`、`WAIST` など)と ID(`TRACKER_1`)、コントローラーに `L` / `R` と ID を表示。ロールを変えると表示も追従
- **視線入力**(SteamVR のアイトラッキングコンポーネント。OpenVR と OpenXR の両方でアプリまで届くことを確認)
- **Windows / Linux** 対応。Linux はダイレクトモード(SteamVR が描いた映像をドライバが受け取る方式)で、実 GPU の Linux 版 SteamVR で動作確認済み
- **ウィンドウなし・低負荷で動かせる**: Windows でもダイレクトモードを選べ、プレビューウィンドウの有無とフレームレート(1〜240 Hz、実行中に変更可)を指定可能。映像は Python から取り出せる(機械学習・自動化向け)
- **Python クライアント**(依存なし)とサンプル、SteamVR 上で動く自動検証スクリプト
- **なりすましなし**: 独自のコントローラタイプ `vhmd_*` で登録。既存アプリのバインディングは SteamVR のリマッピングで流用

> 用途は VR アプリの開発・テスト、アバター / IK / フルトラ挙動の検証、モーキャプデータの流し込みです。
> チート、他人のデバイスのなりすまし、対人環境での不正利用には使わないでください。

## 3 点から始めて、必要な分だけトラッカーを足す

| 構成 | 送るデバイス | 用途の例 |
|---|---|---|
| **3 点** | `hmd`, `left_hand`, `right_hand` | 通常の VR |
| **5 点** | + `left_foot`, `right_foot`(足首) | 足の動き |
| **6 点** | + `waist` | VRChat のフルボディトラッキング |
| 11 点 | + 膝・肘・胸(既定のトラッカー 8 台すべて) | 詳細なモーキャプ |
| 最大 19 点 | `vhmd_config.json` にトラッカーを追加(最大 16 台、肩・手首・足首なども指定可) | |

トラッカーは**送った瞬間に追加**され(実測 約 40 ms)、送信が止まると切断表示になります。SteamVR を再起動するまで登録は残り、再び送れば復帰します。

## はじめかた(ビルド済みバイナリ)

1. [Releases](../../releases) から OS 用のアーカイブを取得して、**動かさない場所**に展開します(展開したフォルダがそのまま SteamVR に登録されます)。
2. 展開したフォルダで登録します(SteamVR の `vrpathreg` を使用。SteamVR 本体へのコピーはしません)。

   | OS | アーカイブ | 登録コマンド |
   |---|---|---|
   | Windows | `VHMD_DevKit-<版>-win64.zip` | `powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1` |
   | Linux | `VHMD_DevKit-<版>-linux64.tar.gz` | `tools/install_driver.sh`(glibc 2.38 以上) |

   登録の前に、SteamVR の設定ファイル(`openvrpaths.vrpath`、`steamvr.vrsettings`、`chaperone_info.vrchap`)と登録済みドライバの一覧を記録します(Windows: `%LOCALAPPDATA%\VHMD_DevKit\history`、Linux: `~/.local/state/vhmd_devkit/history`)。

3. SteamVR を起動し、サンプルを動かします。

   ```bash
   python clients/python/examples/fullbody_sway.py --points 6
   ```

   `--points` は `3` / `5` / `6` / `11`。

4. 解除するときは `tools/uninstall_driver.ps1`(Linux は `.sh`)。登録解除に加えて、ドライバが作ったファイル(ラベル付きのモデルとアイコン)と、SteamVR 設定の vhmd の項目(トラッカーロールなど)を削除し、インストール後に変わったそれ以外の設定を一覧表示します(そちらは変更しません)。
   - `-Restore`(Linux は `--restore`): インストール時に記録した状態にそのまま戻します。SteamVR を閉じてから実行してください。解除の直前の状態も記録されるので、`-Restore -Record <記録>` でさらに元に戻せます。
   - `-List`(`--list`): 記録の一覧。`-KeepSettings` / `-KeepGeneratedFiles`(`--keep-settings` / `--keep-generated-files`)で削除を省けます。

## ソースからビルド

```bash
git clone --recurse-submodules https://github.com/Nekomimi21336/VHMD_DevKit.git
cd VHMD_DevKit
```

### Windows

前提: Visual Studio 2022(C++ によるデスクトップ開発)、CMake 3.20 以上、Git、Steam + SteamVR、Python 3.10 以上。

```bash
cmake --preset vs2022
cmake --build --preset vs2022-release
ctest --preset vs2022-release
powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1
```

- 出力は SteamVR がそのまま読める `build/driver/vhmd/` です。
- **ビルドし直すときは SteamVR を終了してください**(読み込み中の DLL は上書きできません)。
- Visual Studio 2022 しか入っていなければ `cmake -S . -B build -A x64` → `cmake --build build --config Release` でも構いません。新しい Visual Studio が C++ ワークロード無しで入っていると CMake がそちらを選んで失敗するため、プリセットで VS 2022 を指定しています。
- `third_party/openvr`(サブモジュール)が無い場合は、configure 時に OpenVR SDK v2.15.6 を自動取得します。

### Linux

前提: g++ 9 以上、CMake 3.20 以上、Git、Python 3.10 以上、Linux 版 SteamVR。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
tools/install_driver.sh
```

- Linux 版 SteamVR は「デスクトップのウィンドウに描く」方式で起動できないため、Linux ではドライバが SteamVR の描画結果を直接受け取るダイレクトモードで動きます。描画された映像は `python3 clients/python/examples/preview_viewer.py` で見られます。
- 実機での確認は `python3 tests/integration/linux_check.py`(SteamVR の起動〜終了まで自動、レポートを出力)。

詳しくは [docs/linux.md](docs/linux.md)。

## 使い方(Python)

```python
import sys; sys.path.insert(0, "clients/python")   # または: pip install -e clients/python
from vhmd_client import VHMDClient, math3d

with VHMDClient() as c:                            # 127.0.0.1:39570
    with c.batch():                                # まとめて 1 データグラム
        c.pose("hmd", (0, 1.7, 0), math3d.from_euler(yaw=30))
        c.pose("left_hand", (-0.2, 1.2, -0.3))
        c.pose("right_hand", (0.2, 1.2, -0.3))
        c.pose("left_foot", (-0.12, 0.08, 0))      # 初めて送った時点で SteamVR に追加される
        c.pose("right_foot", (0.12, 0.08, 0))
    c.inputs("right_hand", {"trigger": 1.0, "a": True, "joystick_x": 0.5})
    c.skeleton_curl("left_hand", [0, 1, 1, 1, 1])  # 親指以外を握る
    c.gaze(direction=(0.1, 0, -1))
    print(c.status())                              # 登録状態、受信レート、最終受信時刻
```

他の言語からは JSON を UDP で送るだけです。

```json
{ "type": "pose", "id": "left_foot", "pos": [-0.12, 0.08, 0.0], "rot": [1, 0, 0, 0] }
```

| `type` | 内容 |
|---|---|
| `pose` | 位置・回転・速度 |
| `input` | ボタン・トリガー・スティック |
| `skeleton` | 指(曲げ値 or 31 ボーン) |
| `gaze` | 視線 |
| `config` | `list` / `ping` / `add` / `set_role` / `reset` |

- 座標系は SteamVR 準拠(右手系、+Y 上、-Z 前方、メートル)、回転は `[w, x, y, z]`。
- 不正な値(NaN・範囲外・型違いなど)は破棄され、ドライバは止まりません。
- 全仕様: [docs/protocol.md](docs/protocol.md)

## ウィンドウを出さずに使う / FPS を下げる(Windows)

`vhmd_config.json` の `display` を変えると、SteamVR がデスクトップにウィンドウを開かず、ドライバが映像を直接受け取る**ダイレクトモード**になります(Linux と同じ方式)。カメラのライブラリのように、プレビューウィンドウを出すかどうかを選べ、映像は Python から取り出せます。

```jsonc
"display": { "mode": "direct", "refreshHz": 30, "previewWindow": true }
```

| 設定 | 内容 |
|---|---|
| `mode: "direct"` | ダイレクトモード(既定の `auto` は Windows ではウィンドウモード) |
| `refreshHz` | 仮想ディスプレイのフレームレート(1〜240、既定 90)。機械学習や自動化なら 30 や 15 で十分で、SteamVR とアプリの負荷が下がります。実行中も変更できます |
| `previewWindow` | 起動時にプレビューウィンドウ(左右の目の映像)を出す。閉じても SteamVR は止まりません |
| `preview.windowFps` | プレビューウィンドウの更新レート(既定 30) |

実行中の切り替え(`VHMDClient`):

```python
c.refresh_rate(30)        # フレームレートを 30 に(SteamVR を止めずに。引数なしで現在値)
c.preview_window(True)    # プレビューウィンドウを出す / False で隠す
c.preview(True)           # 映像を共有メモリへ書き出す(取り出す側が呼ぶ。止めるときは False)
from vhmd_client.preview import PreviewReader
frame = PreviewReader().read()   # 左右の目を横に並べた RGBA。frame.save_png("eyes.png") など
```

SteamVR のコンポジタは 1〜240 Hz のどの値にも正確に従います。ただし 10 Hz 未満にしてもアプリ側は 12〜15 fps より下がりません(SteamVR が待ちを打ち切るため)。アプリの負荷を下げたい場合の実質的な下限は 15 Hz 前後です。

`python clients/python/examples/preview_viewer.py` でも映像を確認できます。見る人がいないあいだ(ウィンドウを隠している・映像を頼まれていない)は、映像のコピーは一切しません。

負荷の比較(Windows 11、RX 7900 XTX、毎フレーム描画するテストアプリ、CPU は 1 コア比):

| 条件 | アプリの fps | CPU: compositor + vrserver + アプリ | GPU(3D): compositor + vrserver + アプリ |
|---|---|---|---|
| ウィンドウモード 90 Hz(従来) | 66.6 | 32.1 + 5.1 + 1.6 = **38.8 %** | 計測不可 + 0 + 3.7 % |
| ダイレクト 90 Hz(ウィンドウなし) | 89.9 | 10.0 + 6.9 + 1.5 = **18.4 %** | 6.8 + 0 + 2.7 = 9.5 % |
| ダイレクト 90 Hz + プレビューウィンドウ | 89.9 | 10.7 + 8.0 + 2.0 = 20.7 % | 7.1 + 1.5 + 2.5 = 11.1 % |
| ダイレクト 90 Hz + Python へ 15 fps で映像 | 89.9 | 11.2 + 6.6 + 1.8 = 19.6 % | 6.8 + 0.6 + 2.6 = 10.0 % |
| ダイレクト 60 Hz | 59.9 | 7.2 + 7.8 + 1.1 = 16.1 % | 4.6 + 0 + 1.8 = 6.4 % |
| ダイレクト 30 Hz | 30.0 | 5.5 + 7.2 + 0.0 = **12.7 %** | 2.8 + 0 + 0.9 = **3.7 %** |
| ダイレクト 15 Hz | 15.0 | 3.1 + 6.0 + 0.3 = **9.4 %** | 1.3 + 0 + 0.6 = **1.9 %** |

ウィンドウモードはアプリが 66 fps 止まりなうえ、compositor の CPU が高めでした。ダイレクトモードは 90 fps を出しつつ全体で約半分、30 Hz にするとさらに 3 分の 1 程度です(重いアプリほど fps を下げた効果は大きくなります)。詳細: [docs/verification.md](docs/verification.md)

## 設定

| ファイル | 内容 |
|---|---|
| `driver/resources/settings/vhmd_config.json` | ネットワーク、更新周期、タイムアウト(`hold` / `invalid` / `disconnect`)、スムージング、VMT 互換、各デバイスの有効化・ロール・既定ポーズ、`trackersOnDemand` |
| `driver/resources/settings/default.vrsettings` | `driver_vhmd`(有効化、`configFile` で外部設定ファイルを指定)、`vhmd_display`(ウィンドウ / 解像度 / FOV / リフレッシュレート / IPD) |

設定は SteamVR 起動時に読み込まれます。ソース側を編集して再ビルドするか、`configFile` で外部ファイルを指定してください。

**セキュリティ**: 既定では `127.0.0.1` だけで待ち受けます。他の PC から送るには `network.bind` を変更し、`"allowRemote": true` を明示してください。認証は無いので、信頼できるネットワークでのみ使い、ファイアウォールは各自で設定してください。

## 実機 HMD と併用するとき

実機の頭と手(例: Pico 4 Ultra + PICO Connect)に、VHMD のトラッカーを足して使えます(Pico 4 Ultra で確認済み)。

```json
"hmd":         { "enabled": false },
"controllers": { "enabled": false }
```

- 実 HMD のドライバと VHMD は SteamVR の中で並んで動きます(互いにはつながりません)。トラッカーには PC 上の送信プログラムから UDP で姿勢を送ります。
- `activateMultipleDrivers` を変えなくても、VHMD のデバイスは実 HMD と一緒に有効になりました。
- `hmd.enabled` が `true` のままでも、実 HMD があれば SteamVR はそちらを使います(`Using existing HMD`)。VHMD のコントローラーは左右の役割を持たないまま残るので、`controllers.enabled: false` にしておくのがおすすめです。
- 座標は実 HMD の生の座標(`TrackingUniverseRawAndUncalibrated`)に合わせて送ります。送信プログラムで実 HMD の姿勢を読んで計算するか、VMT 互換 OSC の Joint / Follow で実 HMD(`HMD`)を親にします。実 HMD の 50 cm 下にトラッカーを置き続けたところ、頭を動かしても誤差は中央値 0.7 mm でした。
- SteamVR の「スタートアップ/シャットダウン → アドオンの管理」で `vhmd` を無効化することもできます。

## テスト

| 種類 | コマンド | 内容 |
|---|---|---|
| C++ 単体テスト | `ctest --preset vs2022-release` | 53 件(パーサ、座標変換、設定、タイムアウト、ルーター、UDP、ラベル生成、VMT 互換 OSC) |
| Python 単体テスト | `python -m pytest tests/python` | 14 件(クライアント、プレビュー読み取り) |
| Linux キャプチャセルフテスト | `ctest`(Linux) | 実際の Vulkan で共有イメージ → 共有メモリを確認(SteamVR 不要) |
| Linux 実機確認 | `python3 tests/integration/linux_check.py` | ダイレクトモードの表示経路 + 統合テスト、レポート出力 |
| SteamVR 統合テスト | `python tests/integration/run_integration.py` | 58 項目(列挙、オンデマンド追加、誤差、遅延、入力、スケルトン、ハンドモード切り替え、視線、装着センサー、バッテリー、OpenXR のハンドトラッキング、不正パケット、負荷、終了処理)。`vmt.enabled: true` なら VMT 互換 OSC の 9 項目が加わる |
| 起動/終了の繰り返し | `python tests/integration/stress_restart.py --cycles 30` | 毎回ドライバの終了処理とクラッシュ有無を確認 |
| 長時間の安定性 | `python tests/integration/soak_test.py --minutes 30` | フレームレート、姿勢の誤差、メモリ増加、警告を一定間隔で記録 |

統合テストには `pip install openvr` が必要です。起動後に SteamVR を再起動してしまうオーバーレイアプリがある環境では `--stop-relaunchers` を付けてください(Steam 本体やゲームは対象外)。

## ドキュメント

| 内容 | 日本語 | English | 简体中文 | 한국어 |
|---|---|---|---|---|
| README | このページ | [README.en.md](README.en.md) | [README.zh-CN.md](README.zh-CN.md) | [README.ko.md](README.ko.md) |
| プロトコル仕様 | [protocol](docs/protocol.md) | [protocol](docs/en/protocol.md) | [protocol](docs/zh-CN/protocol.md) | [protocol](docs/ko/protocol.md) |
| アーキテクチャ・拡張方法 | [architecture](docs/architecture.md) | [architecture](docs/en/architecture.md) | [architecture](docs/zh-CN/architecture.md) | [architecture](docs/ko/architecture.md) |
| Linux | [linux](docs/linux.md) | [linux](docs/en/linux.md) | [linux](docs/zh-CN/linux.md) | [linux](docs/ko/linux.md) |
| 検証手順と結果 | [verification](docs/verification.md) | — | — | — |
| 判断・未対応事項の記録 | [decisions](docs/decisions.md) | — | — | — |
| 変更履歴 | [CHANGELOG](CHANGELOG.md) | — | — | — |

検証記録・判断記録・変更履歴は開発の記録なので日本語のみです。

## リポジトリ構成

```
CMakeLists.txt / CMakePresets.json   ビルド設定(OpenVR SDK の取得、プラットフォーム別出力)
driver/
  driver.vrdrivermanifest
  resources/            設定、入力プロファイル、リマッピング、レガシーバインディング、アイコン、コントローラーモデル
  src/
    main.cpp            HmdDriverFactory(唯一の公開シンボル)
    device_provider.*   デバイス管理、受信 / 更新スレッド、オンデマンド追加
    device_base.*       全デバイス共通
    hmd_device.*        HMD、仮想ディスプレイ、視線
    linux/              Linux のダイレクトモード、フレームキャプチャ、Vulkan の動的ロード
    windows/            Windows のダイレクトモード(D3D11)、プレビューウィンドウ、共有メモリ
    visuals/            SteamVR に表示するラベル(モデル・アイコン)の生成
    controller_device.* コントローラ、振動、スケルトン
    tracker_device.*    トラッカー、ロール
    math/ protocol/ config/ state/ net/ util/   OpenVR 非依存のコア(単体テスト対象)
clients/python/         vhmd_client パッケージと examples/
tools/                  登録 / 解除 / 環境確認(Windows: .ps1、Linux: .sh)、配布物作成、モデル生成
tests/                  unit/(C++)、python/、integration/(SteamVR)
docs/                   ドキュメント(上の表)
third_party/            OpenVR SDK(サブモジュール v2.15.6)、nlohmann/json 3.11.3
```

## 動作確認環境

| 項目 | バージョン |
|---|---|
| SteamVR | 2.17.10(Windows 11、Linux: Ubuntu 26.04 + NVIDIA RTX 4000 Ada) |
| OpenVR SDK | v2.15.6 |
| Windows コンパイラ | MSVC 14.44(Visual Studio 2022 Build Tools 17.14)、CMake 4.3 |
| Linux コンパイラ | gcc 13.3(Ubuntu 24.04 / WSL2)、gcc 15.2(Ubuntu 26.04)、CMake 3.28 / 4.2 |
| Python | 3.12(3.10 以上)、検証用に pyopenvr 2.12 |

結果の詳細: [docs/verification.md](docs/verification.md)。Linux 版 SteamVR では v0.3.0 のダイレクトモードで、実 GPU(NVIDIA RTX 4000 Ada)上でコンポジタが 90 fps で描画し、統合テスト 28/28、Steam クライアントあり・SteamVR Home 有効での正常終了、Proton 経由の VRChat での描画とコントローラ追従まで確認しました。GTX 1050 では SteamVR のコンポジタが起動後しばらくして GPU 例外で停止します([docs/linux.md](docs/linux.md))。VRChat にログインした状態では、アバターの追従、指、移動、メニュー操作、フルボディ較正を Windows(コントローラー)と Linux(ハンドモード)で確認しました。実 HMD(Pico 4 Ultra + PICO Connect)にトラッカーを足す構成も確認済みです。実画面のある Linux デスクトップ、Linux でのプレビューの開始・停止(軽量化後)は未検証です。変更履歴: [CHANGELOG.md](CHANGELOG.md)

## ライセンス

**BSD 3-Clause License** — Copyright (c) 2026, Nekomimi21336([LICENSE](LICENSE))

| | |
|---|---|
| ✅ 商用利用 | 可 |
| ✅ 改造・派生物の作成 | 可 |
| ✅ 再配布(ソース / ビルド済みバイナリ) | 可 |
| 📄 **義務** | 再配布するときは、著作権表示・条件・免責文([LICENSE](LICENSE) の全文)を、ソースならそのまま、バイナリなら同梱ドキュメント等に含めること |
| 🚫 禁止 | 著作権者の名前を、許可なく派生物の推奨・宣伝に使うこと |
| ⚠ 無保証 | 現状のまま提供され、利用による損害について著作権者は責任を負いません |

上の要約は参考です。正式な条件は [LICENSE](LICENSE) の英文が優先します。

クレジット表記の例(ドキュメントや About 画面などに):

```
This product includes VHMD_DevKit
Copyright (c) 2026, Nekomimi21336 — BSD 3-Clause License
https://github.com/Nekomimi21336/VHMD_DevKit
```

同梱しているサードパーティ(OpenVR SDK: BSD 3-Clause / nlohmann/json: MIT)のライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) にあります。再配布時はこちらも含めてください(配布アーカイブには同梱済み)。
