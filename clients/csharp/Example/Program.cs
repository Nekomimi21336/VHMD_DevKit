// Moves VHMD's headset in a slow circle and pulls the right trigger now and
// then, for 10 seconds; then prints what SteamVR got for the right hand.
//
//     cd clients/csharp/Example && dotnet run

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Threading;
using Vhmd;

class Program
{
    static int Main()
    {
        using (var vhmd = new VhmdClient { Source = "example" })
        {
            if (!vhmd.Ping())
            {
                Console.Error.WriteLine("No answer from the VHMD driver: is SteamVR running in VHMD mode?");
                return 1;
            }
            var clock = Stopwatch.StartNew();
            while (clock.Elapsed.TotalSeconds < 10)
            {
                double t = clock.Elapsed.TotalSeconds;
                double yaw = 0.3 * Math.Sin(t);   // radians, around +Y
                vhmd.Batch(
                    vhmd.PoseJson("hmd", 0.2 * Math.Cos(t), 1.6, 0.2 * Math.Sin(t), Math.Cos(yaw / 2), 0, Math.Sin(yaw / 2), 0),
                    vhmd.PoseJson("right_hand", 0.25, 1.2, -0.3, 1, 0, 0, 0),
                    vhmd.InputsJson("right_hand", new Dictionary<string, double> { { "trigger", (int)t % 2 } }));
                Thread.Sleep(11);   // about 90 a second
            }
            Console.WriteLine(vhmd.Inspect("right_hand"));
        }
        return 0;
    }
}
