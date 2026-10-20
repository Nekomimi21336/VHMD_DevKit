#include "windows/amf_encoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "amf/include/components/VideoEncoderAV1.h"
#include "amf/include/components/VideoEncoderHEVC.h"
#include "amf/include/components/VideoEncoderVCE.h"
#include "amf/include/core/Factory.h"

namespace vhmd {

namespace {
// The runtime is loaded once per process and kept (AMD's samples do the same).
struct Runtime {
    HMODULE dll = nullptr;
    amf::AMFFactory* factory = nullptr;
    std::string error;
};

Runtime& GetRuntime() {
    static Runtime rt = [] {
        Runtime r;
        r.dll = LoadLibraryW(AMF_DLL_NAME);
        if (!r.dll) {
            r.error = "AMF runtime (amfrt64.dll) not found";
            return r;
        }
        auto init = reinterpret_cast<AMFInit_Fn>(GetProcAddress(r.dll, AMF_INIT_FUNCTION_NAME));
        if (!init) {
            r.error = "AMFInit not found";
            return r;
        }
        // Ask for the headers' version, else an older runtime's.
        if (init(AMF_FULL_VERSION, &r.factory) != AMF_OK || !r.factory) {
            r.factory = nullptr;
            if (init(AMF_MAKE_FULL_VERSION(1, 4, 0, 0), &r.factory) != AMF_OK) {
                r.factory = nullptr;
                r.error = "AMFInit failed";
            }
        }
        return r;
    }();
    return rt;
}
} // namespace

struct AmfEncoder::Impl {
    amf::AMFContextPtr context;
    amf::AMFComponentPtr encoder;
    std::string codec;
    uint32_t fps = 72;
    bool ready = false;
};

AmfEncoder::AmfEncoder() : impl_(std::make_unique<Impl>()) {}
AmfEncoder::~AmfEncoder() { Close(); }

bool AmfEncoder::Ready() const { return impl_->ready; }

std::string AmfEncoder::Name() const {
    const std::string c = impl_->codec == "hevc" ? "HEVC" : impl_->codec == "av1" ? "AV1" : "H.264";
    return "AMF " + c + " (ultra low latency)";
}

void AmfEncoder::Close() {
    Impl& m = *impl_;
    if (m.encoder) {
        m.encoder->Terminate();
        m.encoder = nullptr;
    }
    if (m.context) {
        m.context->Terminate();
        m.context = nullptr;
    }
    m.ready = false;
}

bool AmfEncoder::Init(ID3D11Device* device, const std::string& codec, uint32_t width, uint32_t height, uint32_t fps,
                      double bitrate_mbps, std::string* error) {
    Close();
    Impl& m = *impl_;
    Runtime& rt = GetRuntime();
    if (!rt.factory) {
        *error = rt.error;
        return false;
    }
    m.codec = codec;
    m.fps = std::max(1u, fps);
    if (rt.factory->CreateContext(&m.context) != AMF_OK || m.context->InitDX11(device) != AMF_OK) {
        *error = "AMF: no DX11 context on this device";
        Close();
        return false;
    }
    const wchar_t* id = codec == "hevc" ? AMFVideoEncoder_HEVC : codec == "av1" ? AMFVideoEncoder_AV1 : AMFVideoEncoderVCE_AVC;
    if (rt.factory->CreateComponent(m.context, id, &m.encoder) != AMF_OK) {
        *error = "AMF: no " + codec + " encoder on this GPU";
        Close();
        return false;
    }
    const amf_int64 bps = amf_int64(bitrate_mbps * 1e6);
    const amf_int64 gop = amf_int64(m.fps) * 2;
    amf::AMFComponent* e = m.encoder;
    // Usage first: it sets every other property's default.
    if (codec == "hevc") {
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_USAGE, AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_PROFILE, AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMESIZE, ::AMFConstructSize(width, height));
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_FRAMERATE, ::AMFConstructRate(m.fps, 1));
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, bps / m.fps * 2);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_FILLER_DATA_ENABLE, false);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_GOP_SIZE, gop);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE, AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_LOWLATENCY_MODE, true);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT, amf_int64(50));
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_FULL_RANGE_COLOR, false);   // BT.709 limited, like the NV12 input
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
    } else if (codec == "av1") {
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_USAGE, AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMESIZE, ::AMFConstructSize(width, height));
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_FRAMERATE, ::AMFConstructRate(m.fps, 1));
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE, AMF_VIDEO_ENCODER_AV1_ALIGNMENT_MODE_NO_RESTRICTIONS);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE, AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_LOWEST_LATENCY);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, bps / m.fps * 2);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_FILLER_DATA, false);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_GOP_SIZE, gop);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE, AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT, amf_int64(50));
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
    } else {
        e->SetProperty(AMF_VIDEO_ENCODER_USAGE, AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY);
        e->SetProperty(AMF_VIDEO_ENCODER_PROFILE, AMF_VIDEO_ENCODER_PROFILE_HIGH);
        e->SetProperty(AMF_VIDEO_ENCODER_FRAMESIZE, ::AMFConstructSize(width, height));
        e->SetProperty(AMF_VIDEO_ENCODER_FRAMERATE, ::AMFConstructRate(m.fps, 1));
        e->SetProperty(AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD, AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR);
        e->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, bps / m.fps * 2);
        e->SetProperty(AMF_VIDEO_ENCODER_FILLER_DATA_ENABLE, false);
        e->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf_int64(0));
        e->SetProperty(AMF_VIDEO_ENCODER_IDR_PERIOD, gop);
        e->SetProperty(AMF_VIDEO_ENCODER_HEADER_INSERTION_SPACING, gop);
        e->SetProperty(AMF_VIDEO_ENCODER_LOWLATENCY_MODE, true);
        e->SetProperty(AMF_VIDEO_ENCODER_QUERY_TIMEOUT, amf_int64(50));
        e->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_FULL_RANGE_COLOR, false);
        e->SetProperty(AMF_VIDEO_ENCODER_OUTPUT_COLOR_PROFILE, AMF_VIDEO_CONVERTER_COLOR_PROFILE_709);
    }
    if (e->Init(amf::AMF_SURFACE_NV12, amf_int32(width), amf_int32(height)) != AMF_OK) {
        *error = "AMF: the " + codec + " encoder did not start at " + std::to_string(width) + "x" + std::to_string(height);
        Close();
        return false;
    }
    m.ready = true;
    return true;
}

void AmfEncoder::SetBitrate(double mbps) {
    Impl& m = *impl_;
    if (!m.ready) return;
    const amf_int64 bps = amf_int64(mbps * 1e6);
    amf::AMFComponent* e = m.encoder;
    if (m.codec == "hevc") {
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE, bps / m.fps * 2);
    } else if (m.codec == "av1") {
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE, bps / m.fps * 2);
    } else {
        e->SetProperty(AMF_VIDEO_ENCODER_TARGET_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_PEAK_BITRATE, bps);
        e->SetProperty(AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE, bps / m.fps * 2);
    }
}

bool AmfEncoder::Encode(ID3D11Texture2D* nv12, uint64_t pts, bool keyframe, std::vector<uint8_t>* out, bool* is_key) {
    Impl& m = *impl_;
    if (!m.ready) return false;
    amf::AMFSurfacePtr surface;
    if (m.context->CreateSurfaceFromDX11Native(nv12, &surface, nullptr) != AMF_OK) return false;
    surface->SetPts(amf_pts(pts));
    if (keyframe) {
        if (m.codec == "hevc") {
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR);
            surface->SetProperty(AMF_VIDEO_ENCODER_HEVC_INSERT_HEADER, true);
        } else if (m.codec == "av1") {
            surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE, AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY);
            surface->SetProperty(AMF_VIDEO_ENCODER_AV1_FORCE_INSERT_SEQUENCE_HEADER, true);
        } else {
            surface->SetProperty(AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE, AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_SPS, true);
            surface->SetProperty(AMF_VIDEO_ENCODER_INSERT_PPS, true);
        }
    }
    AMF_RESULT r = m.encoder->SubmitInput(surface);
    if (r != AMF_OK && r != AMF_NEED_MORE_INPUT) return false;
    // With the query timeout set, QueryOutput waits for the frame; older
    // runtimes return at once, so poll briefly (ultra low latency: one in, one out).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    amf::AMFDataPtr data;
    for (;;) {
        r = m.encoder->QueryOutput(&data);
        if (r == AMF_OK && data) break;
        if ((r != AMF_REPEAT && r != AMF_OK) || std::chrono::steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    amf::AMFBufferPtr buf(data);
    if (!buf) return false;
    const auto* p = static_cast<const uint8_t*>(buf->GetNative());
    out->assign(p, p + buf->GetSize());
    amf_int64 type = -1;
    if (m.codec == "hevc") {
        buf->GetProperty(AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE, &type);
        *is_key = type == AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR;
    } else if (m.codec == "av1") {
        buf->GetProperty(AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE, &type);
        *is_key = type == AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY;
    } else {
        buf->GetProperty(AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE, &type);
        *is_key = type == AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR;
    }
    return true;
}

} // namespace vhmd
