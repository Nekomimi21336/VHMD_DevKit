// VHMD client for C# (.NET and Unity): sends poses, inputs and finger curls to
// the VHMD driver as JSON over UDP, and asks it questions ("config" requests).
// One file, no packages; C# 7.3 so Unity's compiler takes it as is.
//
// Coordinates are SteamVR's: meters, right handed, +Y up, -Z forward; rotations
// as (w, x, y, z). From Unity (left handed, +Z forward) use FromUnity().
//
//     using (var vhmd = new Vhmd.VhmdClient())
//     {
//         vhmd.Pose("hmd", 0, 1.6, 0, 1, 0, 0, 0);
//         vhmd.Inputs("right_hand", new Dictionary<string, double> { { "trigger", 1 } });
//         string status = vhmd.Request("list");   // the driver's reply, as JSON text
//     }
//
// The message format is docs/protocol.md.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.RegularExpressions;

namespace Vhmd
{
    public sealed class VhmdClient : IDisposable
    {
        public const string DefaultHost = "127.0.0.1";
        public const int DefaultPort = 39570;

        readonly UdpClient udp;
        readonly IPEndPoint driver;
        int seq;

        /// <summary>Sender name ("src"), for the driver's control of who moves a device; null = unnamed.</summary>
        public string Source { get; set; }

        public VhmdClient(string host = DefaultHost, int port = DefaultPort, int timeoutMs = 1000)
        {
            driver = new IPEndPoint(IPAddress.Parse(host), port);
            udp = new UdpClient(new IPEndPoint(IPAddress.Loopback, 0));
            udp.Client.ReceiveTimeout = timeoutMs;
        }

        public void Dispose() { udp.Dispose(); }

        // -- messages -----------------------------------------------------------

        /// <summary>Position (m) and rotation (w, x, y, z) of a device: "hmd",
        /// "left_hand", "right_hand", "tracker_0".. or a tracker role ("waist").</summary>
        public void Pose(string id, double x, double y, double z, double qw, double qx, double qy, double qz)
        {
            Send(PoseJson(id, x, y, z, qw, qx, qy, qz));
        }

        /// <summary>Several devices in one datagram (sent at once, applied together).</summary>
        public void Batch(params string[] messages)
        {
            Send("[" + string.Join(",", messages) + "]");
        }

        public string PoseJson(string id, double x, double y, double z, double qw, double qx, double qy, double qz)
        {
            return "{\"type\":\"pose\",\"id\":" + Str(id) + ",\"pos\":[" + Num(x) + "," + Num(y) + "," + Num(z) +
                   "],\"rot\":[" + Num(qw) + "," + Num(qx) + "," + Num(qy) + "," + Num(qz) + "]" + Src() + "}";
        }

        /// <summary>Buttons and axes by name: "trigger" 0..1, "grip", "joystick_x"/"joystick_y" -1..1,
        /// "a", "b", "x", "y", "system", "menu" (0 or 1) ...</summary>
        public void Inputs(string id, IDictionary<string, double> values)
        {
            Send(InputsJson(id, values));
        }

        public string InputsJson(string id, IDictionary<string, double> values)
        {
            var sb = new StringBuilder("{\"type\":\"input\",\"id\":").Append(Str(id)).Append(",\"values\":{");
            bool first = true;
            foreach (var kv in values)
            {
                if (!first) sb.Append(',');
                sb.Append(Str(kv.Key)).Append(':').Append(Num(kv.Value));
                first = false;
            }
            return sb.Append('}').Append(Src()).Append('}').ToString();
        }

        /// <summary>Finger curls 0 (open) .. 1 (closed): thumb, index, middle, ring, pinky.</summary>
        public void SkeletonCurl(string id, double thumb, double index, double middle, double ring, double pinky)
        {
            Send("{\"type\":\"skeleton\",\"id\":" + Str(id) + ",\"curl\":[" + Num(thumb) + "," + Num(index) + "," +
                 Num(middle) + "," + Num(ring) + "," + Num(pinky) + "]" + Src() + "}");
        }

        /// <summary>Reports a device as not tracked (it greys out in SteamVR).</summary>
        public void Invalidate(string id)
        {
            Send("{\"type\":\"pose\",\"id\":" + Str(id) + ",\"valid\":false" + Src() + "}");
        }

        // -- requests -----------------------------------------------------------

        /// <summary>A config request ("list", "ping", "inspect", ...); returns the reply as JSON
        /// text. `args` is extra JSON fields without braces, e.g. "\"id\":\"left_hand\"".
        /// Throws TimeoutException when the driver does not answer (SteamVR not running).</summary>
        public string Request(string op, string args = null)
        {
            int n = ++seq;
            Send("{\"type\":\"config\",\"op\":" + Str(op) + ",\"seq\":" + n +
                 (string.IsNullOrEmpty(args) ? "" : "," + args) + "}");
            var deadline = DateTime.UtcNow.AddMilliseconds(Math.Max(1, udp.Client.ReceiveTimeout));
            // The driver writes "seq" as a number ("seq":3 or "seq":3.0); other datagrams are haptics.
            var tag = new Regex("\"seq\"\\s*:\\s*" + n + "(\\.0+)?(?![0-9])");
            while (DateTime.UtcNow < deadline)
            {
                IPEndPoint from = null;
                byte[] data;
                try { data = udp.Receive(ref from); }
                catch (SocketException) { break; }
                string text = Encoding.UTF8.GetString(data);
                if (tag.IsMatch(text)) return text;
            }
            throw new TimeoutException("no reply to '" + op + "' from " + driver + " (is SteamVR running?)");
        }

        public bool Ping()
        {
            try { return Regex.IsMatch(Request("ping"), "\"ok\"\\s*:\\s*true"); }
            catch (TimeoutException) { return false; }
        }

        /// <summary>What a device last received and what SteamVR got from it (inputs, finger
        /// bones, haptics): see "inspect" in docs/protocol.md.</summary>
        public string Inspect(string id)
        {
            return Request("inspect", "\"id\":" + Str(id));
        }

        // -- Unity --------------------------------------------------------------

        /// <summary>Unity's left-handed position and rotation (x, y, z, w) to SteamVR's:
        /// z mirrored. Returns x, y, z, qw, qx, qy, qz.</summary>
        public static double[] FromUnity(double px, double py, double pz, double qx, double qy, double qz, double qw)
        {
            return new[] { px, py, -pz, qw, -qx, -qy, qz };
        }

        // -- helpers ------------------------------------------------------------

        void Send(string json)
        {
            byte[] data = Encoding.UTF8.GetBytes(json);
            udp.Send(data, data.Length, driver);
        }

        string Src() { return Source == null ? "" : ",\"src\":" + Str(Source); }

        static string Num(double v)
        {
            if (double.IsNaN(v) || double.IsInfinity(v)) v = 0;   // the driver rejects them
            return v.ToString("R", CultureInfo.InvariantCulture);
        }

        static string Str(string s)
        {
            var sb = new StringBuilder("\"");
            foreach (char c in s)
            {
                if (c == '"' || c == '\\') sb.Append('\\').Append(c);
                else if (c < 0x20) sb.Append("\\u").Append(((int)c).ToString("x4"));
                else sb.Append(c);
            }
            return sb.Append('"').ToString();
        }
    }
}
