# Pico 4 Ultra 向けアプリの調査メモ(2026-10-20)

ヘッドセット側に自作アプリを入れる構成(PICO Connect の代わり、または追加データの送信)を検討するための調査です。Web 上の一次資料(PICO 公式、Khronos、SDK のリリースノート、GitHub)を読んだ結果で、**実機ではまだ何も試していません**。[確認] は一次資料で確認、[二次] は報道・第三者、[未確認] は裏付けなし。

## 結論

- **PICO Connect のまま VHMD を併用する構成で、頭・手 + 仮想トラッカーはすでに使えます**(`docs/verification.md`、実機で確認済み)。
- 自作アプリで得られる**追加のデータ**として価値が高いのは、Motion Tracker の生の姿勢(最大 3 個)と、全身 24 関節のスケルトンです。どちらも PICO Connect では SteamVR に渡りません。
- ただし、トラッキングを受け取れるのは**前面の没入型アプリだけ**と考えるべきで、PICO Connect の実行中に自作アプリから同時に取ることはできない可能性が高い [未確認・最重要]。自作アプリで追加データを取るなら、そのアプリ自体がストリーミングクライアントになる(または ALVR を改造する)必要があります。
- **アイトラッキング・フェイストラッキングのハードウェアはありません**。表情は音声などからの推定値のみ。

## ハードウェア

| 項目 | 内容 |
|---|---|
| SoC / メモリ | Snapdragon XR2 Gen 2、12 GB [確認] |
| ディスプレイ | 2160 × 2160 × 2、90 Hz、視野 105° [確認] |
| カメラ | 32 MP カラーシースルー × 2、iToF 深度 × 1、環境トラッキング × 4 [確認] |
| アイ / フェイストラッキング | なし [確認(スペック表に記載なし)] |
| ハンドトラッキング | あり(SDK 3.0.0 で 60 Hz モード) [確認] |
| Motion Tracker | 両足首 2 個で 24 点の全身推定、腰を足して 3 個、物体トラッキングモードあり [確認] |

## アプリから使える API

| 機能 | API | 備考 |
|---|---|---|
| 全身スケルトン(24 関節) | `XR_BD_body_tracking`(386) | ベンダー拡張。ヘッダーは PICO のサンプルから入手している例あり |
| Motion Tracker の生の姿勢 | ALVR は `XR_BD_motion_tracking` を使用 | Khronos への登録は未確認。物体モードと全身モードは同時に使えない(システム設定で切り替え) [二次] |
| 手 26 関節 | `XR_EXT_hand_tracking` | 実機で動作の報告あり [二次] |
| 表情 | `XR_BD_facial_simulation`(387) | 推定値(音声駆動など) |
| カメラ映像 | `XR_BD_camera_image`(756)、Unity SDK 3.4.0 の `PXR_CameraImage` | 民生機で左右 RGB 640 × 480 / 30 fps が取れた報告 [二次]。以前は個別承認が必要だった。深度は取れない |
| 空間データ | `XR_BD_spatial_sensing` ほか | アンカー、メッシュ、平面 |
| 映像デコード | MediaCodec | AV1 デコード対応 [二次]。解像度・fps の上限は実機で確認が必要 |

- PICO 4 Ultra は OS 5.14 以上が必要な SDK 機能あり。カメラ API や一部の IMU は Enterprise 限定の時期があった。
- 開発者モード: 設定のソフトウェアバージョンを 7 回タップ → USB デバッグ → adb install [二次]。マニフェストで VR アプリとして宣言する必要あり。

## 既存の実装

| 名前 | Pico 4 Ultra | 参考になる点 |
|---|---|---|
| ALVR(MIT) | 対応表に記載あり | Motion Tracker の物体モード(PR #2712)、全身・表情の拡張の使い方 |
| WiVRn(GPL-3.0) | Pico 4 は対応、Ultra は記載なし | コード流用は不可 |
| Steam Link | Pico Store 版が公式対応 | — |
| PicoBridge | 頭・コントローラー・手 26 関節・全身 24 関節を WebSocket で PC に送るデータ専用アプリ | 自作アプリの設計見本(ただし前面アプリである必要がある) |

## 自作アプリを作る場合

VHMD に送れる、PICO Connect では渡らないデータ: Motion Tracker の姿勢、全身 24 関節から合成した腰・膝・肘、手 26 関節(Full のスケルトンとして)、カメラ映像(低解像度)、空間データ。

主なリスク:
- 映像のストリーミング(デコード、リプロジェクション、遅延対策)を自作するのは大仕事。ALVR の改造が近道。
- ベンダー拡張は承認された標準ではなく、ファームウェア更新で変わり得る。
- 民生機と Enterprise で使える機能に差がある。
- ストア配布の審査基準は不明。サイドロード前提が安全。

## 実機で最初に確かめること

1. `xrEnumerateInstanceExtensionProperties` の一覧(どの BD 拡張が使えるか)
2. 自作アプリがフォーカスを失ったとき(PICO Connect の実行中)にトラッキングが止まるか
3. `MediaCodecList` のデコーダーと上限

## 出典

- 公式スペック: https://www.picoxr.com/global/products/pico4-ultra/specs
- Motion Tracker: https://www.picoxr.com/global/products/pico-motion-tracker
- Unity SDK リリース: https://github.com/Pico-Developer/PICO-Unity-Integration-SDK/releases
- Khronos: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_BD_body_tracking.html 、XR_BD_facial_simulation.html 、XR_BD_spatial_sensing.html 、XR_BD_camera_image.html 、XR_BD_dynamic_object_tracking.html
- ALVR: https://github.com/alvr-org/ALVR 、Motion Tracker 対応: https://github.com/alvr-org/ALVR/pull/2712
- WiVRn: https://github.com/WiVRn/WiVRn
- PicoBridge: https://github.com/madderscientist/PicoBridge
- Steam Link(Pico): https://www.khronos.org/news/archives/valve-launches-steam-link-on-pico-headsets-vive-focus-vision-using-openxr
- カメラ API の報告: https://gitcode.csdn.net/69f224a354b52172bc70d186.html 、https://skarredghost.com/2025/03/03/picotionai-ai-mr-camera-access/
