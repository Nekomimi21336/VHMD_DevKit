# C# から使う(.NET と Unity)

VHMD のドライバは、JSON を UDP で `127.0.0.1:39570` に送れば動きます(形式は [docs/protocol.md](../../docs/protocol.md))。`VhmdClient.cs` はそれを C# で書いた 1 ファイルのクライアントで、パッケージは要りません。C# 7.3 で書いてあるので、Unity にもそのまま入れられます。

| ファイル | 中身 |
|---|---|
| `VhmdClient.cs` | 送る(`Pose`、`Inputs`、`SkeletonCurl`、`Invalidate`、まとめて送る `Batch`)と聞く(`Request`、`Ping`、`Inspect`)。Unity の座標から変える `FromUnity` |
| `Example/` | .NET のコンソールの例。ヘッドセットを 10 秒ゆっくり回し、右のトリガーを 1 秒おきに引いてから、SteamVR が受け取った右手の値(`inspect`)を出す |
| `Unity/VhmdPoseSender.cs` | Unity のコンポーネント。付けた GameObject の姿勢を、毎フレーム VHMD の機器(トラッカーなど)として送る |

## .NET の例

SteamVR を VHMD モードで動かしてから:

```bash
cd clients/csharp/Example
dotnet run
```

`net10.0` を対象にしています。古い SDK では `Example.csproj` の `TargetFramework` を手元の版に変えてください。

## Unity

1. `VhmdClient.cs` と `Unity/VhmdPoseSender.cs` を `Assets` に入れる。
2. 動かしたい GameObject に `VhmdPoseSender` を付け、`deviceId` に `tracker_0` や `waist` などを入れる。
3. 再生すると、その GameObject の位置と向きが SteamVR のトラッカーになる。止めると、トラッカーは灰色(追跡なし)になる。

Unity は左手系(+Z が前)、SteamVR は右手系(-Z が前)です。`FromUnity` が Z を反転します(位置は z、回転は x と y の符号)。

## 注意

- 数値はすべて `CultureInfo.InvariantCulture` で書きます(小数点がカンマになる地域でも壊れない)。NaN と無限大は 0 にします(ドライバは受け付けないため)。
- `Source`(`src`)を付けると、ドライバの操作権(`control`)で送り手を区別できます。VHMD Link が操作権を持っているあいだは、ほかの送り手のメッセージは捨てられます。
- 返事は JSON の文字列のまま返します。中身を読むには、Unity なら `JsonUtility`、.NET なら `System.Text.Json` を使ってください。
