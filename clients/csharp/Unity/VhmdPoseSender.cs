// Unity component: sends this GameObject's pose to a VHMD device every frame,
// for example a tracker that follows something in the scene, or the headset
// moved by a test scene. Copy it and ../VhmdClient.cs into the project's Assets.

using UnityEngine;
using Vhmd;

public class VhmdPoseSender : MonoBehaviour
{
    [Tooltip("hmd, left_hand, right_hand, tracker_0.. or a tracker role such as waist")]
    public string deviceId = "tracker_0";
    [Tooltip("Sender name, for the driver's control of who moves a device")]
    public string source = "unity";
    public string host = VhmdClient.DefaultHost;
    public int port = VhmdClient.DefaultPort;

    VhmdClient client;

    void OnEnable()
    {
        client = new VhmdClient(host, port) { Source = source };
    }

    void OnDisable()
    {
        if (client == null) return;
        client.Invalidate(deviceId);   // greys out in SteamVR instead of freezing
        client.Dispose();
        client = null;
    }

    void Update()
    {
        Vector3 p = transform.position;
        Quaternion q = transform.rotation;
        double[] s = VhmdClient.FromUnity(p.x, p.y, p.z, q.x, q.y, q.z, q.w);
        client.Pose(deviceId, s[0], s[1], s[2], s[3], s[4], s[5], s[6]);
    }
}
