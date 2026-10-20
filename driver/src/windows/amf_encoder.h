// Windows: AMD's own encoder API (AMF) in its ultra-low-latency usage, for
// stream_encoder on AMD GPUs. Media Foundation's AMD encoder adds several
// milliseconds per frame; AMF's ultra-low-latency mode encodes each frame as
// soon as it arrives and returns it right away.
//
// The AMF runtime (amfrt64.dll) comes with AMD's graphics driver and is loaded
// at run time: without it (other GPUs, old drivers) Available() is false and
// stream_encoder uses Media Foundation. Headers: third_party/amf (MIT).
//
// One frame at a time, on the caller's thread: Encode() hands an NV12 texture
// to the encoder and waits for its access unit (Annex B, parameter sets on
// keyframes).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace vhmd {

class AmfEncoder {
public:
    AmfEncoder();
    ~AmfEncoder();

    // codec: "h264" | "hevc" | "av1". False (with *error) if AMF or the codec
    // is not available; then nothing is held.
    bool Init(ID3D11Device* device, const std::string& codec, uint32_t width, uint32_t height, uint32_t fps,
              double bitrate_mbps, std::string* error);
    void Close();
    bool Ready() const;
    std::string Name() const;   // e.g. "AMF HEVC (ultra low latency)"

    void SetBitrate(double mbps);
    // Encodes one frame (an NV12 texture of the size given to Init) and waits
    // for it. keyframe: force an IDR / key frame with parameter sets.
    bool Encode(ID3D11Texture2D* nv12, uint64_t pts, bool keyframe, std::vector<uint8_t>* out, bool* is_key);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace vhmd
