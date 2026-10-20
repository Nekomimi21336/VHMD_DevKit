# フェイシャルトラッキング 調査メモ(2026-10-20)

想定する使い方: センサーのない HMD + カメラなどの外部トラッキングで表情を作り、VHMD_DevKit 経由で VR アプリ(まず VRChat)に届ける。将来は、ヘッドセット側で動くアプリが本体のアイ/フェイストラッキングを読んで送る構成も視野に入れる。

> **結論(2026-10-20)**: VHMD_DevKit の役割はソフトウェアと SteamVR の接続なので、SteamVR が受け付けない表情データ(OSC / VRCFT 経由)は**対象外**とする。対象にするのは SteamVR が受け付ける視線(アイトラッキングコンポーネント)だけ。以下は判断材料として残した調査結果。

ここに書いてあることは Web 上の仕様書・公式ドキュメント・ソースコードを読んだ結果で、**まだ何も実装・実機確認していません**。出典は各節の末尾にまとめています。

## 1. 現状(VHMD_DevKit)

- `gaze` メッセージで HMD の視線を 1 本だけ受け取り、`CreateEyeTrackingComponent("/eyetracking")` に渡している(`hmd.eyeTracking`、既定で有効)。
- 使っている OpenVR ヘッダは v2.15.6(公開されている最新)。`VREyeTrackingData_t` の中身は `bActive` / `bValid` / `bTracked` と、視線の始点 `vGazeOrigin`・注視点 `vGazeTarget` だけ。**左右別の視線、まぶたの開き、瞳孔、表情はどこにも無い**。
- 表情(フェイス)を SteamVR に渡す API は OpenVR に**存在しない**。SteamVR の OpenXR ランタイムもフェイス系拡張を提供していない。

→ 表情データは SteamVR の外を通してアプリに届ける必要がある(ALVR、Virtual Desktop、Steam Link なども全部そうしている)。

## 2. 規格

### OpenXR のフェイス系拡張(すべてベンダー拡張)

2026-10-16 の OpenXR 1.1.65 時点で、ベンダー共通(EXT / KHR)のフェイストラッキング拡張は**無い**。視線だけは共通の `XR_EXT_eye_gaze_interaction`(承認済み)がある。

| 拡張 | 中身 | 数 |
|---|---|---|
| `XR_FB_face_tracking2`(Meta) | FACS 準拠の重み + 舌、領域ごとの信頼度、データ源(カメラ / 音声)。Quest Pro はカメラ、Quest 3/3S は音声からの推定のみ | 70 |
| `XR_FB_eye_tracking_social`(Meta) | 左右の目の姿勢と信頼度 | — |
| `XR_ANDROID_face_tracking`(Android XR / Galaxy XR) | Meta とほぼ同じ名前 + 舌、領域ごとの信頼度、状態、キャリブレーション済みフラグ | 68 |
| `XR_HTC_facial_tracking`(HTC) | 目 14 + 口 37、目と口で別々の時刻 | 51 |
| `XR_BD_facial_simulation`(Pico) | ARKit に近い名前 | 52 |
| `XR_ML_facial_expression`(Magic Leap) | FACS の一部 | 46 |

### 表情の語彙(ブレンドシェイプ)

| 語彙 | 数 | 使われる場所 |
|---|---|---|
| **Unified Expressions** | 基本 102(左右別の視線・目の細め/見開き・瞳孔・舌 12 など) | VRCFaceTracking の内部形式、Godot の `XRFaceTracker` |
| Apple ARKit | 52 | iPhone(iFacialMocap、Live Link Face)、VTuber 系の PerfectSync |
| MediaPipe Face Landmarker | 52(ARKit とほぼ同じ名前) | Web カメラでの推定。Apache-2.0 |
| Meta FACS | 63 / 70 | Quest Pro。Android XR の 68 もほぼ同じ |
| HTC(SRanipal 由来) | 目 14 + 口 37 | Vive Facial Tracker |

最も広いのは Unified Expressions。ARKit / Meta / HTC から Unified への変換は VRCFT 内部やコミュニティの対応表にあるが、**公式の対応表は無い**ので、変換表は自前で用意・検証する必要がある。

### 目のデータの表し方

- まぶた: Meta は `EYES_CLOSED` / 見開き / 細め、HTC は `BLINK` / `WIDE` / `SQUEEZE`、VRCFT の OSC は `v2/EyeLid*`(0〜0.75 が開き、0.75〜1 が見開き)+ `v2/EyeSquint*`。
- 瞳孔: VRCFT にはある(`v2/PupilDilation` など)が、OpenXR のフェイス系拡張には無い。
- 座標: OpenXR は右手系・−Z が前。Unity は左手系。OpenVR の視線の座標空間は明記なし(未検証のまま)。

## 3. アプリへの届き方

### VRChat

- **表情のネイティブ API は無い**。表情はアバターパラメータ(OSC)で動かす。
- 目だけはネイティブの OSC 入力がある: `/tracking/eye/CenterPitchYaw`、`LeftRightPitchYaw` など(どれか 1 つ)+ `/tracking/eye/EyesClosedAmount`(両目まとめて 0〜1)。送信先は 9000 番。10 秒途切れると自動の目の動きに戻る。アバター側の特別な設定は不要。

### VRCFaceTracking(VRCFT)

- 事実上の標準の中継ソフト(Apache-2.0、C#)。トラッキング源ごとに**モジュール**(`ExtTrackingModule` を継承した .NET 7 のライブラリ)を作り、モジュールが Unified Expressions の値を書く。VRCFT が `v2/JawOpen` のような名前の OSC アバターパラメータに変換して VRChat に送る(コミュニティでは `FT/v2/...` の接頭辞が一般的)。
- 既存モジュール: ALVR(UDP 41463)、Virtual Desktop(共有メモリ)、Steam Link(OSC 9015、LinkFT)、Pico Streaming Assistant(UDP)、Babble、Quest Link の OpenXR、iPhone / Android の表情アプリなど。**新しいトラッキング源はモジュールを書くのが普通**。
- 注意: 本家の最新版 5.2.3 のリリースノートに「このバージョンはサポートしない」とあり、2026 年はコミュニティのフォークがベータを出している。**保守状況が不透明**。

### その他

| アプリ | 受け取り方 |
|---|---|
| Resonite | 本体で SRanipal、Steam Link の OSC、Babble の OSC に対応 |
| ChilloutVR | 独自の目・口モジュール、OSC。VRCFT 対応は未確認 |
| VTuber 系(VSeeFace、VNyan、Warudo) | VMC プロトコル(`/VMC/Ext/Blend/Val`、ポート 39539)や iFacialMocap 形式 |
| OpenXR アプリ(Unity / Unreal / Godot) | ベンダー拡張を直接読む。SteamVR 上では使えない(VDXR や Quest Link のランタイムでは使える) |

### 既存のストリーミングソフトの経路

| ソフト | 経路 |
|---|---|
| ALVR | ヘッドセット → 映像ストリームに同梱 → PC 側から VRChat の目の OSC か VRCFT モジュールへ UDP |
| Virtual Desktop | 共有メモリ → VRCFT モジュール。別に VDXR(OpenXR ランタイム)が `XR_FB_face_tracking2` を提供 |
| Steam Link | SteamVR が OSC を送る(9000 または 9015)。顔の値が来ないという報告もある |
| Quest Link | Meta の PC 用 OpenXR ランタイムが `XR_FB_face_tracking2` を提供 |

## 4. カメラ系トラッカー(センサーなし HMD 向け)

| トラッカー | 入力 → 出力 | ライセンス |
|---|---|---|
| MediaPipe Face Landmarker | Web カメラ → 52 ブレンドシェイプ(ライブラリ) | Apache-2.0(**送信側の参考実装に最適**) |
| Baballonia / Project Babble | 口カメラ・目カメラ → VRCFT / Resonite / OSC | 非商用のコピーレフト(**コードは流用しない**) |
| EyeTrackVR | ESP32 の IR 目カメラ → OSC / UDP | 同上 |
| iFacialMocap / Live Link Face | iPhone の ARKit → UDP | アプリは非公開 |
| OpenSeeFace | Web カメラ → 目・まばたき・表情(UDP) | BSD-2 |

HMD を被った状態では顔の上半分が隠れるので、Web カメラ単体で取れるのは主に口まわり。目は HMD 内のカメラ(EyeTrackVR など)か、ヘッドセット本体のトラッキングが必要。

## 5. ヘッドセット側アプリ(将来)

| 機種 | 表情 | 目 | API / 権限 |
|---|---|---|---|
| Quest Pro(2024 年末で販売終了) | カメラ | あり | `XR_FB_face_tracking2`、`XR_FB_eye_tracking_social`、`com.oculus.permission.FACE_TRACKING` / `EYE_TRACKING` |
| Quest 3 / 3S | 音声からの推定のみ | なし | `XR_FB_face_tracking2`(データ源 = 音声)、`RECORD_AUDIO` |
| Pico 4 Pro / Enterprise | カメラ(52) | あり | `XR_BD_facial_simulation` など、`com.picovr.permission.FACE_TRACKING` |
| Vive Focus 3 / Focus Vision | 別売の Facial Tracker(60 Hz) | 内蔵 / 別売 | `XR_HTC_facial_tracking` |
| Galaxy XR(Android XR、2025-10) | カメラ(68) | あり | `XR_ANDROID_face_tracking`、`android.permission.FACE_TRACKING`、`EYE_TRACKING_FINE`(実行時に許可が必要) |
| Steam Frame(2025-11 発表) | なし | あり(フォービエイテッド用) | VRChat へはコミュニティのツール経由 |

重要な制約:

- **表情データはフォーカス中のアプリにしか来ない**。Meta の仕様では、フォーカスを失うと値がすべて 0 になる。バックグラウンドのサービスで読む方法はどの機種にも文書が無い。
- つまり、ヘッドセット側アプリは「PC ストリーミングの裏で動く補助アプリ」にはできず、**それ自体がストリーミングクライアント(または ALVR などの改造)になる**のが現実的。ALVR(MIT)と WiVRn(GPL-3.0)はどちらもクライアント内で表情を読み、姿勢と一緒に PC に送っている。
- Meta の開発者ポリシーでは表情・視線は「デバイスユーザーデータ」。ユーザーの明示的な同意が要り、保存しない設計が望ましい。
- 今のドライバは 127.0.0.1 でしか受け付けない。LAN 上のヘッドセットから受けるには、ユーザーが意図して有効にする受信口が必要。

## 6. 設計への示唆(案)

1. **データの入口**: 既存の UDP/JSON に `face` メッセージを足す。
   - 語彙は Unified Expressions を基準にし、ARKit 52 / Meta 70 / Android 68 / HTC / Pico などベンダーの配列も「形式名つき」でそのまま受け付けて、ドライバ側で変換する(ALVR・WiVRn と同じ考え方)。
   - 目は表情とは別に、左右それぞれの視線・開き・見開き・細め・瞳孔を省略可能な項目として持つ。
   - 時刻、有効フラグ、信頼度、データ源(カメラ / 音声)を持てるようにする。
2. **SteamVR への出力**: 左右の視線から 1 本の視線を作って、今の `/eyetracking` に渡す(SteamVR で使える標準の経路はこれだけ)。
3. **アプリへの出力**(どれから作るか要判断):
   - **A. VRCFT モジュール**(普通のやり方): ドライバが共有メモリ(プレビューと同じ方式)か UDP に Unified Expressions を書き、C# の小さなモジュールが読む。VRChat の表情はこれで一通り動く。VRCFT の保守状況に依存する。
   - **B. OSC 直接送信**: VRChat の `/tracking/eye/*` と `EyesClosedAmount` なら依存なしで目が動く。表情まで自前で送ると VRCFT の命名やエンコードを作り直すことになり、VRCFT と同時に使うとぶつかる。
   - **C. 既存形式で出す**: Babble 形式の OSC(Resonite が直接読める)、VMC(VTuber 系)など。
4. **送信側の参考実装**: MediaPipe(Apache-2.0)を使った Python の Web カメラ送信サンプル。HMD を被ると目は取れないので、口まわり中心になる。
5. **ライセンス**: Babble / EyeTrackVR(非商用コピーレフト)と WiVRn(GPL-3.0)のコードは流用しない。プロトコルに合わせるだけなら別問題。

## 未確認・要検証

- OpenVR の視線の座標空間(実機で確認)
- VRCFT の今後(本家とフォークのどちらが主流か)
- Babble の OSC アドレス一覧、Pico Streaming Assistant と iFacialMocap の通信形式
- ChilloutVR、Warudo、NeosVR の受け取り方
- ARKit / Meta → Unified Expressions の変換表(公式のものは無い)
- 各ヘッドセットの更新レート(公表されているのは HTC の 60 Hz のみ)

## 出典

- OpenVR ヘッダ: https://github.com/ValveSoftware/openvr/blob/master/headers/openvr_driver.h 、リリース: https://github.com/ValveSoftware/openvr/releases
- OpenXR 変更履歴: https://raw.githubusercontent.com/KhronosGroup/OpenXR-Docs/main/CHANGELOG.Docs.md
- OpenXR 各拡張: https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrFaceExpression2FB.html 、XrFaceParameterIndicesANDROID.html 、XrEyeExpressionHTC.html 、XrLipExpressionHTC.html 、XrFaceExpressionBD.html 、XrFacialBlendShapeML.html 、XR_FB_eye_tracking_social.html 、XR_EXT_eye_gaze_interaction.html(いずれも同じディレクトリ)
- Android XR: https://developer.android.com/develop/xr/openxr/extensions 、https://developer.android.com/develop/xr/permissions
- Meta: https://developers.meta.com/horizon/documentation/native/android/move-face-tracking/ 、データ利用ポリシー https://developers.meta.com/horizon/policy/data-use/
- SteamVR の OpenXR ランタイムがフェイスに非対応: https://github.com/mbucchia/VirtualDesktop-OpenXR/wiki
- Unified Expressions: https://docs.vrcft.io/docs/tutorial-avatars/tutorial-avatars-extras/unified-blendshapes 、パラメータ: https://docs.vrcft.io/docs/tutorial-avatars/tutorial-avatars-extras/parameters
- VRCFT: https://github.com/benaclejames/VRCFaceTracking 、モジュール SDK: https://docs.vrcft.io/docs/vrcft-software/vrcft-sdk/tracking-module 、フォーク: https://github.com/RealWhyKnot/VRCFaceTracking
- VRChat の目の OSC: https://docs.vrchat.com/docs/osc-eye-tracking 、OSC 全般: https://docs.vrchat.com/docs/osc-overview
- ALVR: https://github.com/alvr-org/ALVR 、VRCFT-ALVR: https://github.com/alvr-org/VRCFT-ALVR
- WiVRn: https://github.com/WiVRn/WiVRn
- Virtual Desktop のモジュール: https://github.com/guygodin/VirtualDesktop.VRCFaceTracking
- LinkFT(Steam Link): https://github.com/ykeara/LinkFT
- oscavmgr: https://github.com/galister/oscavmgr
- Resonite: https://wiki.resonite.com/Face_and_Eye_Tracking_(Setup)
- VMC プロトコル: https://protocol.vmc.info/english
- MediaPipe: https://developers.google.com/edge/mediapipe/solutions/vision/face_landmarker
- Baballonia: https://github.com/Project-Babble/Baballonia 、EyeTrackVR: https://github.com/EyeTrackVR/EyeTrackVR
- Godot XRFaceTracker: https://docs.godotengine.org/en/stable/classes/class_xrfacetracker.html
- 対応表(コミュニティ): https://docs.google.com/spreadsheets/d/118jo960co3Mgw8eREFVBsaJ7z0GtKNr52IB4Bz99VTA/edit
