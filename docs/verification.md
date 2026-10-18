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
