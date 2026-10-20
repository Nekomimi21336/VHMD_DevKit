// Windows: hardware video encoding of the HMD's eye images for streaming to a
// headset (VHMD Link), straight from GPU textures.
//
// Per frame (compositor thread): the eyes, side by side in an RGBA texture, go
// through the D3D11 video processor (scale + RGB -> NV12, BT.709 limited range)
// into a small ring of NV12 textures. An encoder thread feeds those to a Media
// Foundation hardware encoder MFT (H.264 or HEVC; the GPU vendor's, found on the
// same adapter) through a DXGI device manager, so nothing is copied to the CPU
// until the compressed access units come out.
//
// Output: one TCP client at a time on 127.0.0.1:<port> (headset_bridge, which
// forwards it to the headset). Messages use the VHMD Link video framing (see
// clients/python/vhmd_client/headset_stream.py): a 64-byte header
// ("VHV1", kind 1 = config JSON / 2 = access unit, size, flags, frame index,
// time, render pose) and the payload. The client may first send one line of
// JSON with settings: {"bitrate_mbps": 30, "codec": "h264"|"hevc",
// "eye_width": 1920, "fps": 72}. Encoding only runs while a client is connected.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;

namespace vhmd {

struct StreamSettings {
    bool enabled = true;
    uint16_t port = 39583;           // 127.0.0.1 only
    double bitrate_mbps = 30.0;      // defaults; the client's settings line overrides
    std::string codec = "h264";
    uint32_t eye_width = 0;          // 0 = the source eye width (capped to the codec's limit)
    // Each eye's field of view as OpenXR angles (radians: left, right, up, down),
    // sent in the config message so the headset shows the image with it.
    float fov[2][4] = {{-0.87266f, 0.87266f, 0.87266f, -0.87266f}, {-0.87266f, 0.87266f, 0.87266f, -0.87266f}};
};

struct StreamStats {
    bool client = false;
    std::string state = "idle";
    std::string encoder;             // the MFT's friendly name
    uint32_t width = 0, height = 0;
    uint64_t submitted = 0, encoded = 0, dropped = 0, bytes = 0;
};

class StreamEncoder {
public:
    StreamEncoder(ID3D11Device* device, ID3D11DeviceContext* ctx, uint64_t adapter_luid, const StreamSettings& s);
    ~StreamEncoder();

    void Start();
    void Stop();

    // True while a client is connected and frames are wanted.
    bool Wanted() const { return wanted_.load(); }
    void SetRefreshRate(double hz) { fps_ = hz; }
    void SetFov(const float fov[2][4]);

    // Compositor thread: one frame, both eyes side by side (eye_w x eye_h each)
    // in an 8-bit RGBA/BGRA texture holding sRGB-encoded values. pose: the HMD
    // pose it was rendered for (x y z, qw qx qy qz), or nullptr.
    void Submit(ID3D11Texture2D* side_by_side, uint32_t eye_w, uint32_t eye_h, const float* pose);

    StreamStats Stats() const;

private:
    struct Impl;
    void NetLoop();
    void EncodeLoop();

    std::unique_ptr<Impl> impl_;
    StreamSettings settings_;
    std::atomic<double> fps_{90.0};
    std::atomic<bool> running_{false};
    std::atomic<bool> wanted_{false};
    std::thread net_thread_, encode_thread_;
    mutable std::mutex stats_mu_;
    StreamStats stats_;
};

} // namespace vhmd
