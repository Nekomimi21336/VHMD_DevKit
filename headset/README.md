# VHMD Link(ヘッドセット側アプリ)

単体型ヘッドセット(まず Pico 4 Ultra)で動く OpenXR のネイティブアプリです。VHMD を SteamVR の HMD のまま使い、実機のヘッドセットをその「入力」と「表示」にするためのものです。

| 段階 | 内容 | 状態 |
|---|---|---|
| 1 | 頭・両手の姿勢と入力を VHMD に送る(Wi-Fi が基本、USB は予備) | 動作確認済み(Pico 4 Ultra、Pico OS 5.15.7) |
| 2 | VHMD が描いた映像をヘッドセットに表示する | 動作確認済み(最初の版。頭の動きに対する補正はまだ) |
| 3 | ドライバ側でマニュアル(ヘッドセット)とオート(自動操作)を SteamVR を止めずに切り替える | 未着手 |

## しくみ

```
ヘッドセット: VHMD Link ──姿勢・入力(Wi-Fi: UDP 39580 / USB: TCP 39580)──> PC: headset_bridge ──UDP──> VHMD ドライバ
                       <──H.264 映像(TCP 39581)────────────────────────── PC: headset_stream <── VHMD のプレビュー
```

### つなぎ方(Wi-Fi が基本)

1. アプリは起動すると LAN に `VHMD-DISCOVER` をブロードキャスト(UDP 39582)し、PC の `headset_bridge` が答えます。IP アドレスの入力は要りません。
2. 見つかれば Wi-Fi: 姿勢・入力は 1 配列 = 1 データグラムの UDP(遅れた古いパケットで後続が待たされない)、映像は TCP。
3. 見つからなければ USB: `adb reverse` 経由の TCP(127.0.0.1)。1 配列 = 1 行。
4. アドレスを固定したいときは、ヘッドセットの `/sdcard/Android/data/com.vhmd.link/files/pc_address.txt` に PC の IPv4 アドレスを書く(`adb push`)と、探さずにそこへつなぎます。

- ヘッドセット内の床のグリッドの色: 緑 = Wi-Fi、水色 = USB、赤 = 未接続。
- VHMD ドライバは PC 内(127.0.0.1)でだけ待ち受けたままで、LAN に開くのは `headset_bridge` のポート(UDP 39580・39582、TCP 39580・39581)です。LAN 上のどの機器からもつなげるので、信頼できるネットワークで使うか、`--usb-only` で PC 内だけにします。

### 姿勢と入力(段階 1)

- アプリは毎フレーム(72 Hz)、OpenXR の STAGE 空間(床が原点、+Y 上、-Z 前方、メートル)で頭(VIEW)と両手(grip)の姿勢を取り、VHMD のプロトコルのメッセージ(`pose` / `input`)を 1 行の JSON 配列にして送ります。STAGE 空間は SteamVR の立位の座標と同じ向きなので、そのまま送ります。
- 入力: トリガー(値・タッチ・クリック)、グリップ、スティック(x / y・押し込み・タッチ)、A / B / X / Y、メニュー。Pico 4 のコントローラー定義(`XR_BD_controller_interaction`)と Touch の定義の両方でバインドします。
- ヘッドセットの中には床のグリッド(PC につながっていれば緑、つながっていなければ赤)と、コントローラーの軸を表示します。

### 映像(段階 2)

- PC: VHMD のダイレクトモードのプレビュー(両目を並べた RGBA)を ffmpeg に渡し、GPU のエンコーダー(既定 `h264_amf`)で H.264 にする。ffmpeg からは RTP で受け取り、RTP のマーカービットでフレームの終わりが分かった時点で送る(フレームの区切り待ちによる遅れが無い)。
- 色: リミテッドレンジの BT.709 で送る。フルレンジのままだと、ヘッドセットの GPU の YUV 変換がリミテッドレンジとして扱い、暗部がつぶれる(背景 13/255 が 0 になっていた)。
- ヘッドセット: MediaCodec(ハードウェアデコーダー、低遅延モード)で復号して AImageReader に出し、その AHardwareBuffer を EGLImage として外部テクスチャにそのまま貼る(コピーなし)。各目の半分を全面に描き、レイヤーの視野角には VHMD が描いたときの値(PC が SteamVR から取って最初に送る)を使う。
- まだ頭に固定した表示で、描いたときの頭の姿勢での補正(リプロジェクション)はしていない。頭を動かすと遅れの分だけ映像がずれる。

## 使い方

1. インストールのときだけ USB が要ります: ヘッドセットで開発者モードと USB デバッグを有効にし、USB でつなぐ(`adb devices` に出ること)。PC とヘッドセットは同じ LAN に。
2. ビルドとインストール(初回は `--fetch-loader` で OpenXR ローダーを Maven Central から取得):

   ```powershell
   python headset/android/build_apk.py --fetch-loader --install
   ```

3. PC で中継を起動(Wi-Fi の待ち受けと、USB 用の `adb reverse` を両方行います):

   ```powershell
   python -m vhmd_client.headset_bridge            # USB を使わないなら --no-adb、Wi-Fi を使わないなら --usb-only
   ```

4. SteamVR は VHMD モード(`python -m vhmd_client.mode vhmd`)にしておくと、VHMD の HMD とコントローラーがヘッドセットに合わせて動きます。

- FPS 操作ツールなど、ほかに HMD・手の姿勢を送るプログラムを同時に動かすと、両方の値が混ざります(段階 3 で切り替えられるようにします)。

## ビルド

Gradle は使いません。`build_apk.py` が次を行います。

- NDK + CMake(Ninja)で `libvhmdlink.so` をビルド
- `aapt2 link` で AndroidManifest.xml から APK を作り、`.so` を無圧縮で追加して `zipalign -P 16`
- ローカルの Android デバッグ鍵(`~/.android/debug.keystore`)で `apksigner` 署名

必要なもの: Android SDK(NDK・build-tools・platform)、CMake、Ninja、JDK。OpenXR ローダーは Khronos の `openxr_loader_for_android-1.1.63.aar`(SHA-1 を確認)を `headset/.cache` に置きます(Git 管理外)。

## 確認したこと(2026-10-21、Pico 4 Ultra A9210、Pico OS 5.15.7、Android 14)

- Khronos のローダーがシステムのランタイムブローカー経由で Pico のランタイム(`com.pico.xr.openxr_runtime`)を読み込み、拡張 69 個。Pico 4 のコントローラー定義あり。
- 目ごとのスワップチェーンは 1920 × 1920。セッションは FOCUSED まで進み、72 回/秒で PC に届く(約 58 KiB/s)。
- VHMD モードの SteamVR で、`VHMD-HMD-0`・`VHMD-CTL-L/R` がヘッドセットの姿勢になった。
- 映像: 2160 × 1200(両目並び)を 20〜26 Mbit/s で送り、ヘッドセットで受信・復号 約 60 回/秒、表示の更新 約 52 回/秒(表示は 72 Hz)。PC 側の入力が約 60 回/秒で頭打ち(プレビューを Python でコピーして ffmpeg に渡している部分)。ヘッドセットの画面キャプチャと PC の元画像で、向き・左右・明るさが一致。
- Wi-Fi(5 GHz、Wi-Fi 5、866 Mbit/s、RSSI -32 dBm): USB の転送を消した状態で、アプリが PC(192.168.0.4)を自動で見つけ、姿勢・入力 72 回/秒(UDP)、映像 約 63 fps・20 Mbit/s(TCP)。映像を流している間の往復遅延(ping)は平均 2 ms(1〜4 ms)。何も流していないときは平均 38 ms(ヘッドセットの Wi-Fi 省電力)。
- USB だけ(`--usb-only`)でも従来どおり動く。
- 映像を出すには、VHMD の設定で `display.mode = "direct"`、`preview.fps` を 90、`preview.maxEyeWidth` を 1200 にした(既定の 15 fps / 960 では遅く小さい)。
