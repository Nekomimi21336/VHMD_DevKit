# VHMD_DevKit — 開発用 仮想HMD / フルトラッキング SteamVR ドライバ

実機の HMD・コントローラ・トラッカーが無くても、外部プロセス(Python 等)から UDP/JSON で送ったポーズ・入力を SteamVR に「実機があるかのように」見せる**開発・テスト用**ドライバです(Windows / Linux)。

**基本は 3 点 VR**(HMD + 左右コントローラ)。トラッカーは**送った時点で追加**され、そのまま同期します。足首 2 本を送れば 5 点、腰も足せば 6 点(VRChat のフルトラ)、最大 16 台まで。

- 仮想 HMD 1 台(ダミーディスプレイ、視線入力)
- 仮想コントローラ 2 台(トリガー/グリップ/スティック/A・B・X・Y/システム/メニュー、振動、指スケルトン)
- 仮想ジェネリックトラッカー(オンデマンド追加、既定カタログ 8 台・最大 16、腰・両足・両膝・両肘・胸など、ロール指定可)
- UDP + JSON の外部入力 API(バッチ更新可)、任意で VMT 互換 OSC
- Python クライアント(依存なし)とサンプル、SteamVR 上での自動検証スクリプト

> 用途は VR アプリ開発・テスト、アバター/IK/フルトラ挙動の検証、モーキャプデータの流し込みです。
> 製品配布、チート、他人のデバイスのなりすまし、対人環境での不正利用には使わないでください。
> 独自のコントローラタイプ名 (`vhmd_*`) で登録し、他社製品のなりすましはしません。

## 動作確認済みバージョン

| 項目 | バージョン |
|---|---|
| OpenVR SDK | **v2.15.6**(`third_party/openvr`、CMake 変数 `VHMD_OPENVR_TAG` で固定) |
| SteamVR | **2.17.10**(buildid 25330290) |
| Windows | 11 Pro 10.0.26200 |
| コンパイラ | MSVC 14.44(Visual Studio 2022 Build Tools 17.14) |
| CMake | 4.3.0-rc2(3.20 以上で動作する構成) |
| Python | 3.12.7(3.10 以上)、pyopenvr 2.12.1401(検証用) |

検証結果の詳細は [docs/verification.md](docs/verification.md)。

## ビルド済みバイナリで使う(ビルド不要)

[Releases](../../releases) から OS 用のアーカイブを取得して展開し、中の `vhmd` フォルダを登録します。

- Windows: `VHMD_DevKit-<版>-win64.zip` を展開 → そのフォルダで `powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1`
- Linux: `VHMD_DevKit-<版>-linux64.tar.gz` を展開 → そのフォルダで `tools/install_driver.sh`(glibc 2.38 以上。古い環境はソースからビルド。[docs/linux.md](docs/linux.md))

展開したフォルダがそのまま SteamVR に登録されるので、**展開先は後で動かさない場所**にしてください(動かしたら登録し直し)。

アーカイブには `tools/` と Python クライアント・サンプルも入っています。

## クイックスタート(ソースからビルド)

前提: Visual Studio 2022(C++ デスクトップ開発)、CMake 3.20+、Git、Steam + SteamVR、Python 3.10+。

```bash
git clone --recurse-submodules <このリポジトリ>
cd VirtualHMD
```

1. 環境チェック(読み取りのみ):

```bash
powershell -ExecutionPolicy Bypass -File tools/verify_env.ps1
```

2. ビルド(出力は SteamVR がそのまま読める `build/driver/vhmd/`):

```bash
cmake -S . -B build -A x64
```

```bash
cmake --build build --config Release
```

```bash
ctest --test-dir build -C Release --output-on-failure
```

3. SteamVR に登録(`vrpathreg adddriver`。SteamVR 本体へのコピーやシステム設定変更はしません):

```bash
powershell -ExecutionPolicy Bypass -File tools/install_driver.ps1
```

4. SteamVR を起動し、サンプルを実行:

```bash
python clients/python/examples/fullbody_sway.py --points 6
```

(`--points 3` で 3 点、`5` で足首まで、`6` で腰まで、`11` で既定トラッカー全部)

```bash
python clients/python/examples/circle_motion.py
```

```bash
python clients/python/examples/gamepad.py --demo
```

5. 自動検証(SteamVR の起動〜終了まで自動):

```bash
pip install openvr
```

```bash
python tests/integration/run_integration.py --stop-relaunchers
```

6. 登録解除(完全に元に戻す。`-CleanSettings` はドライバが SteamVR 設定に書いたトラッカーロール等もバックアップ付きで削除):

```bash
powershell -ExecutionPolicy Bypass -File tools/uninstall_driver.ps1 -CleanSettings
```

> **ビルドし直すときは SteamVR を終了してください**(読み込み中の DLL は上書きできません)。

> **Visual Studio が複数入っている場合**: CMake は最新の Visual Studio を自動選択します。この PC のように VS 2026 (18) が C++ ワークロード無しで入っていると `No CMAKE_CXX_COMPILER could be found` で失敗します。その場合はジェネレータを明示してください:
>
> ```bash
> cmake -S . -B build -G "Visual Studio 17 2022" -A x64
> ```
>
> または同梱のプリセット: `cmake --preset vs2022` → `cmake --build --preset vs2022-release` → `ctest --preset vs2022-release`
>
> `third_party/openvr` が無い(サブモジュール未取得)場合、CMake が configure 時に v2.15.6 を自動取得します。

## Linux

Linux 版 SteamVR 向けのビルド・登録手順と実機確認チェックリストは [docs/linux.md](docs/linux.md)。Linux ビルドと単体テストは WSL2 (Ubuntu 24.04, gcc 13.3) で確認済み、**Linux 版 SteamVR 上での動作は未検証**です。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && tools/install_driver.sh
```

## 実機 HMD がある PC での注意

- このドライバの HMD は `hmd_presence: ["*.*"]` で常に登録されます。実 HMD(PICO 等)のドライバと同時に有効だと、どちらがアクティブ HMD になるかは環境依存です(この PC では PICO 未接続時に vhmd が選ばれることを確認)。
- 実機で遊ぶ前に `tools/uninstall_driver.ps1` で登録解除するか、`vhmd_config.json` で `"hmd": {"enabled": false}` にして**トラッカー/コントローラだけ**を追加するモードにしてください(このモードは実 HMD 接続環境での確認が未実施です)。
- SteamVR の「スタートアップ/シャットダウン → アドオンの管理」から `vhmd` を無効化することもできます。

## Python から使う

```python
import sys; sys.path.insert(0, "clients/python")   # または: pip install -e clients/python
from vhmd_client import VHMDClient, math3d

with VHMDClient() as c:                       # 127.0.0.1:39570
    with c.batch():                           # まとめて 1 データグラム
        c.pose("hmd", (0, 1.7, 0), math3d.from_euler(yaw=30))     # 3 点(常に登録済み)
        c.pose("left_hand", (-0.2, 1.2, -0.3))
        c.pose("right_hand", (0.2, 1.2, -0.3))
        c.pose("left_foot", (-0.12, 0.08, 0))  # ← 初めて送った時点で SteamVR に追加される
        c.pose("right_foot", (0.12, 0.08, 0))  #    (ロール名でも tracker_N でも可)
    c.inputs("right_hand", {"trigger": 1.0, "a": True, "joystick_x": 0.5})
    c.skeleton_curl("left_hand", [0, 1, 1, 1, 1])   # 親指以外を握る
    c.gaze(direction=(0.1, 0, -1))
    print(c.status())                         # 登録デバイス、受信レート、最終受信時刻
```

プロトコル仕様: [docs/protocol.md](docs/protocol.md)(座標系は SteamVR 準拠: 右手系・Y 上・-Z 前方・メートル、回転は `[w,x,y,z]`)。

## 設定

| ファイル | 内容 |
|---|---|
| `driver/resources/settings/vhmd_config.json` | ネットワーク、更新周期、タイムアウト(`hold`/`invalid`/`disconnect`)、スムージング、VMT 互換、HMD/コントローラ/トラッカーの有効化・台数・ロール・既定ポーズ |
| `driver/resources/settings/default.vrsettings` | `driver_vhmd`(有効化、`configFile` で別の設定ファイルを指定)、`vhmd_display`(ウィンドウ位置/解像度/FOV/リフレッシュレート/IPD) |

設定は SteamVR 起動時に読み込まれます。ビルドすると `build/driver/vhmd/` にコピーされるので、編集はソース側(`driver/resources/...`)で行い再ビルドするか、`configFile` で外部ファイルを指定してください。

### セキュリティ

既定では **127.0.0.1 のみ**で待ち受けます。他 PC から送る場合は `network.bind` を変更し `"allowRemote": true` を明示する必要があります(認証は無いので、信頼できるネットワークでのみ使用してください。ファイアウォールの設定は各自で行ってください)。

## リポジトリ構成

```
CMakeLists.txt              ルート(OpenVR SDK 取得・プラットフォーム設定)
driver/
  driver.vrdrivermanifest
  resources/                設定、入力プロファイル、リマッピング、レガシーバインディング、アイコン
  src/
    main.cpp                HmdDriverFactory(唯一の export)
    device_provider.*       IServerTrackedDeviceProvider、受信/更新スレッド
    device_base.*           全デバイス共通(ポーズ発行、プロパティ)
    hmd_device.*            HMD + IVRDisplayComponent + 視線
    controller_device.*     コントローラ + 振動 + スケルトン
    tracker_device.*        トラッカー + ロール
    math/ protocol/ config/ state/ net/ util/   OpenVR 非依存コア (vhmd_core)
clients/python/             vhmd_client パッケージ、examples/
tools/                      install/uninstall/verify_env (Windows: .ps1 / Linux: .sh)、gen_remapping.py
tests/unit/                 C++ 単体テスト(36 件)
tests/python/               Python クライアント単体テスト(pytest, 8 件)
tests/integration/          SteamVR 統合テスト、入力/スケルトン検証、起動終了ストレス
docs/                       protocol / architecture / verification / decisions
third_party/                openvr (submodule, v2.15.6), nlohmann/json 3.11.3
```

設計の詳細と拡張方法: [docs/architecture.md](docs/architecture.md)、判断・未対応事項: [docs/decisions.md](docs/decisions.md)。

## ライセンス

BSD 3-Clause([LICENSE](LICENSE))。同梱物のライセンスは [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
