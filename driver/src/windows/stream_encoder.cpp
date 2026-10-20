#include "windows/stream_encoder.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <initguid.h>   // CODECAPI_* GUIDs for this file (selectany)
#include <audioclient.h>
#include <codecapi.h>
#include <mmdeviceapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/log.h"

using Microsoft::WRL::ComPtr;

namespace vhmd {
namespace {

constexpr int kRing = 4;               // NV12 textures between the video processor and the encoder
constexpr uint32_t kKindConfig = 1, kKindFrame = 2, kKindAudio = 3, kKindAudioFormat = 4;

#pragma pack(push, 1)
struct VideoHeader {
    char magic[4];
    uint32_t kind;
    uint32_t size;
    uint32_t flags;      // bit 0: pose valid
    uint64_t frame_index;
    uint64_t time_us;
    float pose[7];
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(VideoHeader) == 64, "VHMD Link video header");

uint64_t NowUs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ull) / 10;
}

bool IsBgraFamily(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_TYPELESS;
}

// Does this access unit already carry the parameter sets (SPS for H.264, VPS for HEVC)?
bool HasParameterSets(const uint8_t* p, size_t n, bool hevc) {
    for (size_t i = 0; i + 3 < n; ++i) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            const uint8_t h = p[i + 3];
            const int type = hevc ? (h >> 1) & 0x3F : h & 0x1F;
            if (hevc ? type == 32 : type == 7) return true;
            i += 2;
        }
    }
    return false;
}

void SetCodecValue(ICodecAPI* api, const GUID& key, ULONG v) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = v;
    api->SetValue(&key, &var);
}

void SetCodecBool(ICodecAPI* api, const GUID& key, bool v) {
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = v ? VARIANT_TRUE : VARIANT_FALSE;
    api->SetValue(&key, &var);
}

} // namespace

struct StreamEncoder::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    uint64_t luid = 0;

    // Video processor (compositor thread).
    ComPtr<ID3D11VideoDevice> vdev;
    ComPtr<ID3D11VideoContext> vctx;
    ComPtr<ID3D11VideoProcessorEnumerator> vpe;
    ComPtr<ID3D11VideoProcessor> vp;
    ID3D11Texture2D* vp_src = nullptr;     // texture the input view was made for
    ComPtr<ID3D11VideoProcessorInputView> in_view;
    uint32_t in_w = 0, in_h = 0, out_w = 0, out_h = 0;
    DXGI_FORMAT in_fmt = DXGI_FORMAT_UNKNOWN;
    ComPtr<ID3D11Texture2D> nv12[kRing];
    ComPtr<ID3D11VideoProcessorOutputView> out_view[kRing];

    // Frames waiting for the encoder.
    struct Frame {
        int slot;
        uint64_t index;
        LONGLONG time;     // 100 ns
        float pose[7];
        bool pose_ok;
        uint32_t w, h;
    };
    std::mutex q_mu;
    std::condition_variable q_cv;
    std::deque<Frame> queue;
    bool slot_busy[kRing] = {};
    uint64_t next_index = 0;
    std::deque<Frame> in_flight;           // given to the encoder, waiting for output

    // Encoder (encode thread).
    ComPtr<IMFTransform> mft;
    ComPtr<IMFMediaEventGenerator> events;
    ComPtr<ICodecAPI> codec_api;
    ComPtr<IMFDXGIDeviceManager> dxgi_mgr;
    UINT reset_token = 0;
    DWORD in_id = 0, out_id = 0;
    bool provides_samples = false;
    uint32_t enc_w = 0, enc_h = 0;
    std::vector<uint8_t> seq_header;
    int need_input = 0;
    bool hevc = false;
    double enc_fps = 0;

    // Client (net thread accepts, encode thread sends).
    std::mutex client_mu;
    SOCKET client = INVALID_SOCKET;
    SOCKET listener = INVALID_SOCKET;
    bool config_sent = false;
    std::atomic<bool> keyframe{false};
    // Settings for the current client.
    std::mutex set_mu;
    double bitrate_mbps = 30;
    std::string codec = "h264";
    uint32_t eye_width = 0;
    int buffer = 0;
    float fov[2][4] = {};

    // One message to the client (any thread). False if there is no client
    // or it is gone.
    bool Send(uint32_t kind, uint64_t index, const uint8_t* a, size_t na, const uint8_t* b, size_t nb,
              const float* pose) {
        VideoHeader h{};
        std::memcpy(h.magic, "VHV1", 4);
        h.kind = kind;
        h.size = uint32_t(na + nb);
        h.flags = pose ? 1 : 0;
        h.frame_index = index;
        h.time_us = NowUs();
        if (pose) std::memcpy(h.pose, pose, sizeof(h.pose));
        std::lock_guard<std::mutex> l(client_mu);
        if (client == INVALID_SOCKET) return false;
        WSABUF bufs[3] = {{ULONG(sizeof(h)), reinterpret_cast<CHAR*>(&h)},
                          {ULONG(na), reinterpret_cast<CHAR*>(const_cast<uint8_t*>(a))},
                          {ULONG(nb), reinterpret_cast<CHAR*>(const_cast<uint8_t*>(b))}};
        DWORD sent = 0;
        if (WSASend(client, bufs, 3, &sent, 0, nullptr, nullptr) != 0 || sent != sizeof(h) + na + nb) {
            closesocket(client);
            client = INVALID_SOCKET;
            return false;
        }
        return true;
    }
};

StreamEncoder::StreamEncoder(ID3D11Device* device, ID3D11DeviceContext* ctx, uint64_t adapter_luid,
                             const StreamSettings& s)
    : impl_(std::make_unique<Impl>()), settings_(s) {
    impl_->device = device;
    impl_->ctx = ctx;
    impl_->luid = adapter_luid;
    impl_->bitrate_mbps = s.bitrate_mbps;
    impl_->codec = s.codec;
    impl_->eye_width = s.eye_width;
    std::memcpy(impl_->fov, s.fov, sizeof(impl_->fov));
}

StreamEncoder::~StreamEncoder() { Stop(); }

void StreamEncoder::SetFov(const float fov[2][4]) {
    std::lock_guard<std::mutex> l(impl_->set_mu);
    std::memcpy(impl_->fov, fov, sizeof(impl_->fov));
}

void StreamEncoder::Start() {
    if (!settings_.enabled || running_.exchange(true)) return;
    net_thread_ = std::thread(&StreamEncoder::NetLoop, this);
    encode_thread_ = std::thread(&StreamEncoder::EncodeLoop, this);
    if (settings_.audio) audio_thread_ = std::thread(&StreamEncoder::AudioLoop, this);
}

void StreamEncoder::Stop() {
    if (!running_.exchange(false)) return;
    {
        std::lock_guard<std::mutex> l(impl_->client_mu);
        if (impl_->listener != INVALID_SOCKET) closesocket(impl_->listener);
        if (impl_->client != INVALID_SOCKET) closesocket(impl_->client);
        impl_->listener = impl_->client = INVALID_SOCKET;
    }
    impl_->q_cv.notify_all();
    if (net_thread_.joinable()) net_thread_.join();
    if (encode_thread_.joinable()) encode_thread_.join();
    if (audio_thread_.joinable()) audio_thread_.join();
}

StreamStats StreamEncoder::Stats() const {
    std::lock_guard<std::mutex> l(stats_mu_);
    return stats_;
}

// --- compositor thread --------------------------------------------------------
void StreamEncoder::Submit(ID3D11Texture2D* src, uint32_t eye_w, uint32_t eye_h, const float* pose) {
    if (!wanted_) return;
    Impl& m = *impl_;
    D3D11_TEXTURE2D_DESC sd{};
    src->GetDesc(&sd);

    // Output size: the eye width asked for (or the source's), even and within the
    // codec's limit (H.264 4096 wide in total, HEVC 8192), aspect kept.
    uint32_t want_eye;
    bool hevc;
    {
        std::lock_guard<std::mutex> l(m.set_mu);
        want_eye = m.eye_width ? m.eye_width : eye_w;
        hevc = m.codec == "hevc";
    }
    want_eye = std::min({want_eye, eye_w, hevc ? 4096u : 2048u});
    const uint32_t out_w = (want_eye * 2) & ~15u;
    const uint32_t out_h = uint32_t(std::lround(double(eye_h) * want_eye / eye_w)) & ~15u;
    if (out_w < 64 || out_h < 64) return;

    if (!m.vdev && (FAILED(m.device.As(&m.vdev)) || FAILED(m.ctx.As(&m.vctx)))) {
        std::lock_guard<std::mutex> l(stats_mu_);
        stats_.state = "error: no D3D11 video device";
        return;
    }
    // (Re)create the video processor and the NV12 ring when sizes change.
    if (!m.vp || m.in_w != sd.Width || m.in_h != sd.Height || m.out_w != out_w || m.out_h != out_h ||
        m.in_fmt != sd.Format) {
        std::unique_lock<std::mutex> ql(m.q_mu);
        for (bool b : m.slot_busy)
            if (b) return;   // the encoder still has frames of the old size: try next frame
        ql.unlock();
        m.vp.Reset();
        m.vpe.Reset();
        m.in_view.Reset();
        m.vp_src = nullptr;
        const UINT fps = UINT(std::lround(fps_.load()));
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
        cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        cd.InputFrameRate = {fps, 1};
        cd.InputWidth = sd.Width;
        cd.InputHeight = sd.Height;
        cd.OutputFrameRate = {fps, 1};
        cd.OutputWidth = out_w;
        cd.OutputHeight = out_h;
        cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
        if (FAILED(m.vdev->CreateVideoProcessorEnumerator(&cd, &m.vpe)) ||
            FAILED(m.vdev->CreateVideoProcessor(m.vpe.Get(), 0, &m.vp))) {
            std::lock_guard<std::mutex> l(stats_mu_);
            stats_.state = "error: video processor";
            return;
        }
        // Input: full-range RGB holding sRGB-encoded values; output: BT.709 limited.
        ComPtr<ID3D11VideoContext1> vctx1;
        if (SUCCEEDED(m.vctx.As(&vctx1))) {
            vctx1->VideoProcessorSetStreamColorSpace1(m.vp.Get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            vctx1->VideoProcessorSetOutputColorSpace1(m.vp.Get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        } else {
            D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
            in.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
            out.YCbCr_Matrix = 1;
            out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
            m.vctx->VideoProcessorSetStreamColorSpace(m.vp.Get(), 0, &in);
            m.vctx->VideoProcessorSetOutputColorSpace(m.vp.Get(), &out);
        }
        m.vctx->VideoProcessorSetStreamAutoProcessingMode(m.vp.Get(), 0, FALSE);
        for (int i = 0; i < kRing; ++i) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = out_w;
            td.Height = out_h;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_NV12;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            m.nv12[i].Reset();
            m.out_view[i].Reset();
            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
            ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
            if (FAILED(m.device->CreateTexture2D(&td, nullptr, &m.nv12[i])) ||
                FAILED(m.vdev->CreateVideoProcessorOutputView(m.nv12[i].Get(), m.vpe.Get(), &ov, &m.out_view[i]))) {
                m.vp.Reset();
                std::lock_guard<std::mutex> l(stats_mu_);
                stats_.state = "error: NV12 textures";
                return;
            }
        }
        m.in_w = sd.Width;
        m.in_h = sd.Height;
        m.in_fmt = sd.Format;
        m.out_w = out_w;
        m.out_h = out_h;
    }
    if (m.vp_src != src) {
        m.in_view.Reset();
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
        iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(m.vdev->CreateVideoProcessorInputView(src, m.vpe.Get(), &iv, &m.in_view))) {
            std::lock_guard<std::mutex> l(stats_mu_);
            stats_.state = "error: video processor input (format " + std::to_string(sd.Format) + ")";
            return;
        }
        m.vp_src = src;
    }
    int slot = -1;
    {
        std::lock_guard<std::mutex> l(m.q_mu);
        for (int i = 0; i < kRing; ++i)
            if (!m.slot_busy[i]) {
                slot = i;
                break;
            }
        if (slot < 0) {   // the encoder is behind: skip this frame
            std::lock_guard<std::mutex> s(stats_mu_);
            ++stats_.dropped;
            return;
        }
        m.slot_busy[slot] = true;
    }
    const RECT in_rect{0, 0, LONG(eye_w * 2), LONG(eye_h)};
    const RECT out_rect{0, 0, LONG(out_w), LONG(out_h)};
    m.vctx->VideoProcessorSetStreamSourceRect(m.vp.Get(), 0, TRUE, &in_rect);
    m.vctx->VideoProcessorSetStreamDestRect(m.vp.Get(), 0, TRUE, &out_rect);
    m.vctx->VideoProcessorSetOutputTargetRect(m.vp.Get(), TRUE, &out_rect);
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = m.in_view.Get();
    if (FAILED(m.vctx->VideoProcessorBlt(m.vp.Get(), m.out_view[slot].Get(), 0, 1, &st))) {
        std::lock_guard<std::mutex> l(m.q_mu);
        m.slot_busy[slot] = false;
        return;
    }
    m.ctx->Flush();

    Impl::Frame f{};
    f.slot = slot;
    f.w = out_w;
    f.h = out_h;
    f.pose_ok = pose != nullptr;
    if (pose) std::memcpy(f.pose, pose, sizeof(f.pose));
    {
        std::lock_guard<std::mutex> l(m.q_mu);
        f.index = ++m.next_index;
        f.time = LONGLONG(f.index) * 100000;   // 10 ms steps: only used to pair outputs with frames
        m.queue.push_back(f);
    }
    m.q_cv.notify_one();
    std::lock_guard<std::mutex> s(stats_mu_);
    ++stats_.submitted;
}

// --- network thread -----------------------------------------------------------
void StreamEncoder::NetLoop() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(settings_.port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    BOOL excl = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&excl), sizeof(excl));
    if (ls == INVALID_SOCKET || bind(ls, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(ls, 1) != 0) {
        Log(LogLevel::Warning, "stream: cannot listen on 127.0.0.1:%u", unsigned(settings_.port));
        if (ls != INVALID_SOCKET) closesocket(ls);
        std::lock_guard<std::mutex> l(stats_mu_);
        stats_.state = "error: port " + std::to_string(settings_.port) + " in use";
        WSACleanup();
        return;
    }
    {
        std::lock_guard<std::mutex> l(impl_->client_mu);
        impl_->listener = ls;
    }
    Log(LogLevel::Info, "stream: encoder output on 127.0.0.1:%u", unsigned(settings_.port));
    while (running_) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) break;
        BOOL nd = TRUE;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nd), sizeof(nd));
        int sndbuf = 4 << 20;
        setsockopt(c, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));
        // Optional settings line from the client (300 ms).
        DWORD tmo = 300;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
        std::string line;
        char ch;
        while (line.size() < 1024 && recv(c, &ch, 1, 0) == 1 && ch != '\n') line += ch;
        if (!line.empty()) {
            auto j = nlohmann::json::parse(line, nullptr, false);
            if (j.is_object()) {
                std::lock_guard<std::mutex> l(impl_->set_mu);
                if (j.contains("bitrate_mbps") && j["bitrate_mbps"].is_number())
                    impl_->bitrate_mbps = std::clamp(j["bitrate_mbps"].get<double>(), 1.0, 500.0);
                if (j.contains("codec") && j["codec"].is_string())
                    impl_->codec = j["codec"].get<std::string>() == "hevc" ? "hevc" : "h264";
                if (j.contains("eye_width") && j["eye_width"].is_number())
                    impl_->eye_width = uint32_t(std::clamp(j["eye_width"].get<int>(), 0, 4096));
                if (j.contains("buffer") && j["buffer"].is_number())
                    impl_->buffer = std::clamp(j["buffer"].get<int>(), 0, 3);
            }
        }
        {
            std::lock_guard<std::mutex> l(impl_->client_mu);
            impl_->client = c;
            impl_->config_sent = false;
        }
        impl_->keyframe = true;
        wanted_ = true;
        {
            std::lock_guard<std::mutex> l(stats_mu_);
            stats_.client = true;
        }
        Log(LogLevel::Info, "stream: client connected (%s)", line.empty() ? "default settings" : line.c_str());
        // The client sends nothing more; a read returning 0 / error means it left.
        tmo = 0;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tmo), sizeof(tmo));
        while (running_ && recv(c, &ch, 1, 0) > 0) {
        }
        wanted_ = false;
        {
            std::lock_guard<std::mutex> l(impl_->client_mu);
            if (impl_->client == c) {
                closesocket(c);
                impl_->client = INVALID_SOCKET;
            }
        }
        impl_->q_cv.notify_all();
        {
            std::lock_guard<std::mutex> l(stats_mu_);
            stats_.client = false;
        }
        Log(LogLevel::Info, "stream: client left");
    }
    {
        std::lock_guard<std::mutex> l(impl_->client_mu);
        if (impl_->listener != INVALID_SOCKET) closesocket(impl_->listener);
        impl_->listener = INVALID_SOCKET;
    }
    WSACleanup();
}

// --- encode thread ------------------------------------------------------------
void StreamEncoder::EncodeLoop() {
    Impl& m = *impl_;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        std::lock_guard<std::mutex> l(stats_mu_);
        stats_.state = "error: MFStartup";
        return;
    }
    auto send_msg = [&](uint32_t kind, uint64_t index, const uint8_t* a, size_t na, const uint8_t* b, size_t nb,
                        const float* pose) { return m.Send(kind, index, a, na, b, nb, pose); };

    auto teardown = [&]() {
        if (m.mft) {
            m.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
            m.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
            ComPtr<IMFShutdown> sd;
            if (SUCCEEDED(m.mft.As(&sd))) sd->Shutdown();
        }
        m.mft.Reset();
        m.events.Reset();
        m.codec_api.Reset();
        m.dxgi_mgr.Reset();
        m.enc_w = m.enc_h = 0;
        m.need_input = 0;
        std::lock_guard<std::mutex> l(m.q_mu);
        for (auto& f : m.in_flight) m.slot_busy[f.slot] = false;
        for (auto& f : m.queue) m.slot_busy[f.slot] = false;
        m.in_flight.clear();
        m.queue.clear();
    };

    auto setup = [&](uint32_t w, uint32_t h) -> bool {
        double bitrate;
        {
            std::lock_guard<std::mutex> l(m.set_mu);
            bitrate = m.bitrate_mbps;
            m.hevc = m.codec == "hevc";
        }
        const GUID subtype = m.hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
        MFT_REGISTER_TYPE_INFO in_info{MFMediaType_Video, MFVideoFormat_NV12}, out_info{MFMediaType_Video, subtype};
        IMFActivate** acts = nullptr;
        UINT32 count = 0;
        ComPtr<IMFAttributes> enum_attrs;
        MFCreateAttributes(&enum_attrs, 1);
        LUID luid{DWORD(m.luid & 0xffffffffu), LONG(m.luid >> 32)};
        enum_attrs->SetBlob(MFT_ENUM_ADAPTER_LUID, reinterpret_cast<const UINT8*>(&luid), sizeof(luid));
        MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in_info, &out_info,
                 enum_attrs.Get(), &acts, &count);
        if (count == 0) {
            CoTaskMemFree(acts);
            acts = nullptr;
            MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in_info,
                      &out_info, &acts, &count);
        }
        if (count == 0) {
            CoTaskMemFree(acts);
            std::lock_guard<std::mutex> l(stats_mu_);
            stats_.state = std::string("error: no hardware ") + (m.hevc ? "HEVC" : "H.264") + " encoder";
            return false;
        }
        WCHAR* wname = nullptr;
        UINT32 wlen = 0;
        std::string name;
        if (SUCCEEDED(acts[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wname, &wlen))) {
            char buf[256] = {};
            WideCharToMultiByte(CP_UTF8, 0, wname, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
            name = buf;
            CoTaskMemFree(wname);
        }
        HRESULT hr = acts[0]->ActivateObject(IID_PPV_ARGS(&m.mft));
        for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
        CoTaskMemFree(acts);
        if (FAILED(hr)) return false;

        ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(m.mft->GetAttributes(&attrs))) {
            attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }
        if (FAILED(m.mft.As(&m.events))) return false;
        if (m.mft->GetStreamIDs(1, &m.in_id, 1, &m.out_id) == E_NOTIMPL) m.in_id = m.out_id = 0;

        if (FAILED(MFCreateDXGIDeviceManager(&m.reset_token, &m.dxgi_mgr)) ||
            FAILED(m.dxgi_mgr->ResetDevice(m.device.Get(), m.reset_token)) ||
            FAILED(m.mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m.dxgi_mgr.Get())))) {
            std::lock_guard<std::mutex> l(stats_mu_);
            stats_.state = "error: encoder does not take D3D11 textures";
            return false;
        }

        const UINT32 fps = UINT32(std::lround(fps_.load()));
        m.enc_fps = fps;
        // Rate control before the media types: some encoders fix it when the
        // output type is set. Set again afterwards for the others.
        auto set_rate = [&]() {
            if (!m.codec_api && FAILED(m.mft.As(&m.codec_api))) return;
            SetCodecValue(m.codec_api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
            SetCodecValue(m.codec_api.Get(), CODECAPI_AVEncCommonMeanBitRate, ULONG(bitrate * 1e6));
            SetCodecValue(m.codec_api.Get(), CODECAPI_AVEncCommonMaxBitRate, ULONG(bitrate * 1e6));
            SetCodecValue(m.codec_api.Get(), CODECAPI_AVEncCommonBufferSize, ULONG(bitrate * 1e6 / fps * 2));
            SetCodecBool(m.codec_api.Get(), CODECAPI_AVLowLatencyMode, true);
            SetCodecValue(m.codec_api.Get(), CODECAPI_AVEncMPVGOPSize, fps * 2);
            SetCodecValue(m.codec_api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);
        };
        set_rate();
        ComPtr<IMFMediaType> out_type;
        MFCreateMediaType(&out_type);
        out_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out_type->SetGUID(MF_MT_SUBTYPE, subtype);
        out_type->SetUINT32(MF_MT_AVG_BITRATE, UINT32(bitrate * 1e6));
        MFSetAttributeSize(out_type.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(out_type.Get(), MF_MT_FRAME_RATE, fps, 1);
        MFSetAttributeRatio(out_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        out_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        out_type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
        out_type->SetUINT32(MF_MT_MPEG2_PROFILE, m.hevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High);
        if (FAILED(hr = m.mft->SetOutputType(m.out_id, out_type.Get(), 0))) {
            std::lock_guard<std::mutex> l(stats_mu_);
            stats_.state = "error: encoder output type (" + std::to_string(hr) + ")";
            return false;
        }
        ComPtr<IMFMediaType> in_type;
        for (DWORD i = 0; SUCCEEDED(m.mft->GetInputAvailableType(m.in_id, i, &in_type)); ++i) {
            GUID sub{};
            in_type->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12) break;
            in_type.Reset();
        }
        if (!in_type) MFCreateMediaType(&in_type);
        in_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        MFSetAttributeSize(in_type.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(in_type.Get(), MF_MT_FRAME_RATE, fps, 1);
        in_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        in_type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
        if (FAILED(hr = m.mft->SetInputType(m.in_id, in_type.Get(), 0))) {
            std::lock_guard<std::mutex> l(stats_mu_);
            stats_.state = "error: encoder input type (" + std::to_string(hr) + ")";
            return false;
        }
        set_rate();
        MFT_OUTPUT_STREAM_INFO osi{};
        m.mft->GetOutputStreamInfo(m.out_id, &osi);
        m.provides_samples = (osi.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        m.seq_header.clear();
        ComPtr<IMFMediaType> cur;
        if (SUCCEEDED(m.mft->GetOutputCurrentType(m.out_id, &cur))) {
            UINT8* blob = nullptr;
            UINT32 n = 0;
            if (SUCCEEDED(cur->GetAllocatedBlob(MF_MT_MPEG_SEQUENCE_HEADER, &blob, &n))) {
                m.seq_header.assign(blob, blob + n);
                CoTaskMemFree(blob);
            }
        }
        m.mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m.mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        m.enc_w = w;
        m.enc_h = h;
        m.need_input = 0;
        Log(LogLevel::Info, "stream: %s %ux%u @ %u fps, %.0f Mbit/s, encoder '%s'", m.hevc ? "HEVC" : "H.264", w, h, fps,
            bitrate, name.c_str());
        std::lock_guard<std::mutex> l(stats_mu_);
        stats_.state = "encoding";
        stats_.encoder = name;
        stats_.width = w;
        stats_.height = h;
        return true;
    };

    auto send_config = [&]() {
        nlohmann::json j;
        {
            std::lock_guard<std::mutex> l(m.set_mu);
            j["codec"] = m.hevc ? "hevc" : "h264";
            j["width"] = m.enc_w;
            j["height"] = m.enc_h;
            j["eye_width"] = m.enc_w / 2;
            j["fov"] = {{m.fov[0][0], m.fov[0][1], m.fov[0][2], m.fov[0][3]},
                        {m.fov[1][0], m.fov[1][1], m.fov[1][2], m.fov[1][3]}};
            j["pose"] = true;
            j["buffer"] = m.buffer;
            j["source"] = "driver";
        }
        const std::string s = j.dump();
        if (send_msg(kKindConfig, 0, reinterpret_cast<const uint8_t*>(s.data()), s.size(), nullptr, 0, nullptr)) {
            std::lock_guard<std::mutex> l(m.client_mu);
            m.config_sent = true;
        }
    };

    auto drain_output = [&]() {
        for (;;) {
            MFT_OUTPUT_DATA_BUFFER out{};
            out.dwStreamID = m.out_id;
            ComPtr<IMFSample> own;
            if (!m.provides_samples) {
                MFT_OUTPUT_STREAM_INFO osi{};
                m.mft->GetOutputStreamInfo(m.out_id, &osi);
                ComPtr<IMFMediaBuffer> b;
                MFCreateSample(&own);
                MFCreateMemoryBuffer(std::max<DWORD>(osi.cbSize, 4 << 20), &b);
                own->AddBuffer(b.Get());
                out.pSample = own.Get();
            }
            DWORD status = 0;
            HRESULT hr = m.mft->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents) out.pEvents->Release();
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> t;
                if (SUCCEEDED(m.mft->GetOutputAvailableType(m.out_id, 0, &t))) m.mft->SetOutputType(m.out_id, t.Get(), 0);
                continue;
            }
            if (FAILED(hr) || !out.pSample) {
                if (out.pSample && !own) out.pSample->Release();
                return;
            }
            IMFSample* sample = out.pSample;
            LONGLONG t = 0;
            sample->GetSampleTime(&t);
            UINT32 clean = 0;
            sample->GetUINT32(MFSampleExtension_CleanPoint, &clean);
            ComPtr<IMFMediaBuffer> buf;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf))) {
                BYTE* data = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                    // The frame this output belongs to (and every older one) is done.
                    Impl::Frame f{};
                    bool found = false;
                    {
                        std::lock_guard<std::mutex> l(m.q_mu);
                        while (!m.in_flight.empty() && m.in_flight.front().time <= t) {
                            if (m.in_flight.front().time == t) {
                                f = m.in_flight.front();
                                found = true;
                            }
                            m.slot_busy[m.in_flight.front().slot] = false;
                            m.in_flight.pop_front();
                        }
                    }
                    const bool add_header = clean && !m.seq_header.empty() && !HasParameterSets(data, len, m.hevc);
                    bool config_ok;
                    {
                        std::lock_guard<std::mutex> l(m.client_mu);
                        config_ok = m.config_sent;
                    }
                    if (config_ok)
                        send_msg(kKindFrame, found ? f.index : 0, add_header ? m.seq_header.data() : nullptr,
                                 add_header ? m.seq_header.size() : 0, data, len, found && f.pose_ok ? f.pose : nullptr);
                    buf->Unlock();
                    std::lock_guard<std::mutex> l(stats_mu_);
                    ++stats_.encoded;
                    stats_.bytes += len;
                }
            }
            if (!own) sample->Release();
            return;   // one output per HaveOutput event
        }
    };

    while (running_) {
        if (!wanted_) {
            if (m.mft) teardown();
            std::unique_lock<std::mutex> l(m.q_mu);
            m.q_cv.wait_for(l, std::chrono::milliseconds(100));
            continue;
        }
        // A client is there: set the encoder up for the size of the frames coming in.
        if (!m.mft) {
            uint32_t w = 0, h = 0;
            {
                std::unique_lock<std::mutex> l(m.q_mu);
                m.q_cv.wait_for(l, std::chrono::milliseconds(100), [&] { return !m.queue.empty() || !wanted_ || !running_; });
                if (m.queue.empty()) continue;
                w = m.queue.back().w;
                h = m.queue.back().h;
            }
            if (!setup(w, h)) {
                teardown();
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            send_config();
            {   // frames that piled up while the encoder started are stale: keep the newest
                std::lock_guard<std::mutex> l(m.q_mu);
                while (m.queue.size() > 1) {
                    m.slot_busy[m.queue.front().slot] = false;
                    m.queue.pop_front();
                }
            }
        }
        bool reconfigure = false;
        {
            std::lock_guard<std::mutex> l(m.q_mu);
            reconfigure = !m.queue.empty() && (m.queue.back().w != m.enc_w || m.queue.back().h != m.enc_h);
        }
        bool want_codec_change;
        {
            std::lock_guard<std::mutex> l(m.set_mu);
            want_codec_change = (m.codec == "hevc") != m.hevc;
        }
        if (reconfigure || want_codec_change || std::lround(fps_.load()) != std::lround(m.enc_fps)) {
            teardown();
            continue;
        }
        bool need_config;
        {
            std::lock_guard<std::mutex> l(m.client_mu);
            need_config = !m.config_sent && m.client != INVALID_SOCKET;
        }
        if (need_config) send_config();

        ComPtr<IMFMediaEvent> ev;
        HRESULT hr = m.events->GetEvent(MF_EVENT_FLAG_NO_WAIT, &ev);
        if (hr == MF_E_NO_EVENTS_AVAILABLE) {
            std::unique_lock<std::mutex> l(m.q_mu);
            m.q_cv.wait_for(l, std::chrono::milliseconds(1));
        } else if (FAILED(hr)) {
            teardown();
            continue;
        } else {
            MediaEventType type = MEUnknown;
            ev->GetType(&type);
            if (type == METransformNeedInput) ++m.need_input;
            else if (type == METransformHaveOutput) drain_output();
        }
        // Feed queued frames while the encoder asks for input.
        while (m.need_input > 0) {
            Impl::Frame f{};
            {
                std::lock_guard<std::mutex> l(m.q_mu);
                if (m.queue.empty()) break;
                f = m.queue.front();
                m.queue.pop_front();
                m.in_flight.push_back(f);
            }
            ComPtr<IMFMediaBuffer> buf;
            ComPtr<IMFSample> sample;
            if (FAILED(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), m.nv12[f.slot].Get(), 0, FALSE, &buf))) {
                std::lock_guard<std::mutex> l(m.q_mu);
                m.slot_busy[f.slot] = false;
                m.in_flight.pop_back();
                continue;
            }
            ComPtr<IMF2DBuffer> b2;
            DWORD len = 0;
            if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->GetContiguousLength(&len))) buf->SetCurrentLength(len);
            MFCreateSample(&sample);
            sample->AddBuffer(buf.Get());
            sample->SetSampleTime(f.time);
            sample->SetSampleDuration(100000);
            if (m.keyframe.exchange(false) && m.codec_api) SetCodecValue(m.codec_api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
            if (FAILED(m.mft->ProcessInput(m.in_id, sample.Get(), 0))) {
                std::lock_guard<std::mutex> l(m.q_mu);
                m.slot_busy[f.slot] = false;
                m.in_flight.pop_back();
                break;
            }
            --m.need_input;
        }
    }
    teardown();
    MFShutdown();
}

// --- audio thread -------------------------------------------------------------
// Loopback of the default output device while a client is connected. Polled
// every 5 ms (event-driven loopback needs Windows 10 1703+ and gains little at
// this period); converted to interleaved 16-bit, at most 2 channels, the
// device's own rate (the headset resamples).
void StreamEncoder::AudioLoop() {
    Impl& m = *impl_;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto set_state = [&](const std::string& st) {
        std::lock_guard<std::mutex> l(stats_mu_);
        stats_.audio = st;
    };
    while (running_) {
        if (!wanted_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> ac;
        ComPtr<IAudioCaptureClient> cap;
        WAVEFORMATEX* wf = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
            FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &ac)) || FAILED(ac->GetMixFormat(&wf)) ||
            FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000 /* 20 ms */, 0, wf,
                                  nullptr)) ||
            FAILED(ac->GetService(IID_PPV_ARGS(&cap)))) {
            if (wf) CoTaskMemFree(wf);
            set_state("error: no loopback capture");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        // Float32 (the usual mix format) or 16-bit integer.
        bool is_float = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
            is_float = ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
        }
        const UINT32 rate = wf->nSamplesPerSec;
        const UINT32 in_ch = wf->nChannels;
        const UINT32 bits = wf->wBitsPerSample;
        CoTaskMemFree(wf);
        if (!((is_float && bits == 32) || (!is_float && bits == 16))) {
            set_state("error: unsupported mix format (" + std::to_string(bits) + " bit)");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        const UINT32 out_ch = std::min<UINT32>(2, in_ch);
        const std::string fmt = "{\"rate\":" + std::to_string(rate) + ",\"channels\":" + std::to_string(out_ch) +
                                ",\"format\":\"s16\"}";
        m.Send(kKindAudioFormat, 0, reinterpret_cast<const uint8_t*>(fmt.data()), fmt.size(), nullptr, 0, nullptr);
        ac->Start();
        set_state("capturing " + std::to_string(rate) + " Hz");
        Log(LogLevel::Info, "stream: audio loopback %u Hz, %u channel(s) -> %u", rate, in_ch, out_ch);
        std::vector<int16_t> out;
        uint64_t sample_no = 0;
        const size_t chunk = size_t(rate) / 100 * out_ch;   // 10 ms
        while (running_ && wanted_) {
            UINT32 n = 0;
            if (FAILED(cap->GetNextPacketSize(&n))) break;
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            for (UINT32 f = 0; f < frames; ++f) {
                for (UINT32 c = 0; c < out_ch; ++c) {
                    int16_t v = 0;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        if (is_float) {
                            const float x = reinterpret_cast<const float*>(data)[size_t(f) * in_ch + c];
                            v = int16_t(std::clamp(x, -1.0f, 1.0f) * 32767.0f);
                        } else {
                            v = reinterpret_cast<const int16_t*>(data)[size_t(f) * in_ch + c];
                        }
                    }
                    out.push_back(v);
                }
            }
            cap->ReleaseBuffer(frames);
            while (out.size() >= chunk) {
                if (!m.Send(kKindAudio, sample_no, reinterpret_cast<const uint8_t*>(out.data()), chunk * 2, nullptr, 0,
                            nullptr))
                    break;
                sample_no += chunk / out_ch;
                out.erase(out.begin(), out.begin() + std::ptrdiff_t(chunk));
                std::lock_guard<std::mutex> l(stats_mu_);
                ++stats_.audio_packets;
            }
        }
        ac->Stop();
        set_state("off");
    }
    if (SUCCEEDED(co)) CoUninitialize();
}

} // namespace vhmd
