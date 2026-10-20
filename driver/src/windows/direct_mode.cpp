#include "windows/direct_mode.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

#include "util/log.h"
#include "util/precise_sleep.h"
#include "util/preview_shm.h"

using Microsoft::WRL::ComPtr;

namespace vhmd {
namespace {

// Formats the preview path understands (8-bit RGBA / BGRA families).
bool IsBgra(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
           f == DXGI_FORMAT_B8G8R8X8_UNORM || f == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB || f == DXGI_FORMAT_B8G8R8X8_TYPELESS;
}
bool IsRgba(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R8G8B8A8_UNORM || f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || f == DXGI_FORMAT_R8G8B8A8_TYPELESS;
}
// Typed format for views / mip generation (typeless -> sRGB, the compositor writes sRGB-encoded data).
DXGI_FORMAT Typed(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
    default: return f;
    }
}
// Same family, UNORM: what a flip-model swap chain accepts (bits are copied as-is).
DXGI_FORMAT SwapFormat(DXGI_FORMAT f) { return IsBgra(f) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM; }

std::wstring MappingName(const std::string& path) {
    std::string n = path;
    if (const auto slash = n.find_last_of("/\\"); slash != std::string::npos) n = n.substr(slash + 1);
    if (n.empty()) n = "vhmd_preview";
    return std::wstring(n.begin(), n.end());
}

uint64_t RealtimeNs() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const uint64_t t = (uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; // 100 ns since 1601
    return (t - 116444736000000000ull) * 100;
}

} // namespace

struct DirectModeComponent::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGIFactory2> factory;

    struct Tex {
        ComPtr<ID3D11Texture2D> tex;
        uint32_t pid = 0;
        vr::SharedTextureHandle_t first = 0; // handle identifying the set
        uint32_t set_index = 0;              // 0..2 inside the set
    };
    std::map<vr::SharedTextureHandle_t, Tex> textures; // guarded by mu
    std::map<vr::SharedTextureHandle_t, uint32_t> set_next; // first handle -> next index
    std::mutex mu;

    // Sync texture from the compositor (keyed mutex).
    vr::SharedTextureHandle_t sync_handle = 0;
    ComPtr<IDXGIKeyedMutex> sync_mutex;

    // Preview: both eyes side by side with a mip chain, mip `level` is read back / shown.
    ComPtr<ID3D11Texture2D> atlas;
    ComPtr<ID3D11ShaderResourceView> atlas_srv;
    DXGI_FORMAT atlas_format = DXGI_FORMAT_UNKNOWN;
    uint32_t atlas_w = 0, atlas_h = 0;
    uint32_t shm_level = 0, win_level = 0;
    std::chrono::steady_clock::time_point next_shm{}, next_win{};   // frame-rate cap deadlines

    // Shared-memory readback (2 staging textures, mapped by the readback thread).
    ComPtr<ID3D11Texture2D> staging[2];
    float staging_pose[2][7] = {};   // render pose of what each staging texture holds
    // Video for VHMD Link: both eyes side by side, UNORM so the video processor takes it.
    ComPtr<ID3D11Texture2D> stream_src;
    uint32_t stream_w = 0, stream_h = 0;
    DXGI_FORMAT stream_fmt = DXGI_FORMAT_UNKNOWN;
    bool staging_pose_ok[2] = {};
    int staging_pending = -1; // index waiting to be read back, -1 = none (guarded by mu)
    uint32_t staging_w = 0, staging_h = 0;
    std::condition_variable readback_cv;
    HANDLE mapping = nullptr;
    uint8_t* view = nullptr;
    size_t view_size = 0;
    uint64_t frame_index = 0;

    // Window
    std::atomic<bool> window_frame_ready{false};
    std::atomic<uint64_t> window_frames{0};
    HWND hwnd = nullptr;

    void ReleaseShm() {
        if (view) UnmapViewOfFile(view);
        if (mapping) CloseHandle(mapping);
        view = nullptr;
        mapping = nullptr;
        view_size = 0;
    }
};

DirectModeComponent::DirectModeComponent(double refresh_hz, const CaptureSettings& capture, bool window_visible,
                                         double window_fps)
    : refresh_hz_(refresh_hz > 1.0 ? refresh_hz : 90.0), capture_(capture),
      window_fps_(std::clamp(window_fps, 1.0, 240.0)), impl_(std::make_unique<Impl>()) {
    preview_active_ = capture.enabled && capture.start_active;
    window_visible_ = window_visible;
    stats_.capture.state = !capture.enabled ? "disabled" : preview_active_ ? "waiting for first frame" : "paused";
}

DirectModeComponent::~DirectModeComponent() {
    Stop();
    impl_->ReleaseShm();
}

bool DirectModeComponent::Init(std::string& error) {
    ComPtr<IDXGIFactory1> f1;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f1)))) return error = "CreateDXGIFactory1 failed", false;
    f1.As(&impl_->factory);
    ComPtr<IDXGIAdapter1> adapter;
    const UINT index = capture_.vulkan_device >= 0 ? UINT(capture_.vulkan_device) : 0;
    if (FAILED(f1->EnumAdapters1(index, &adapter))) return error = "DXGI adapter " + std::to_string(index) + " not found", false;
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    adapter_luid_ = (uint64_t(uint32_t(ad.AdapterLuid.HighPart)) << 32) | ad.AdapterLuid.LowPart;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    // Video support: the stream encoder's video processor needs it; retried without
    // for adapters that do not offer it (the stream is then unavailable).
    HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                                   UINT(std::size(levels)), D3D11_SDK_VERSION, &impl_->device, nullptr, &impl_->ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                               UINT(std::size(levels)), D3D11_SDK_VERSION, &impl_->device, nullptr, &impl_->ctx);
    if (FAILED(hr)) return error = "D3D11CreateDevice failed (" + std::to_string(hr) + ")", false;
    // The compositor thread, the window thread and the readback thread share the context.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(impl_->ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    char name[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
    Log(LogLevel::Info, "direct mode: D3D11 on adapter %u '%s'", index, name);
    if (capture_.stream.enabled) {
        stream_ = std::make_unique<StreamEncoder>(impl_->device.Get(), impl_->ctx.Get(), adapter_luid_, capture_.stream);
        stream_->SetRefreshRate(refresh_hz_);
    }
    return true;
}

void DirectModeComponent::Start() {
    if (running_.exchange(true)) return;
    if (stream_) stream_->Start();
    vsync_thread_ = std::thread(&DirectModeComponent::VsyncLoop, this);
    window_thread_ = std::thread(&DirectModeComponent::WindowLoop, this);
    readback_thread_ = std::thread(&DirectModeComponent::ReadbackLoop, this);
    Log(LogLevel::Info, "direct mode: vsync clock started at %.1f Hz", refresh_hz_.load());
}

void DirectModeComponent::Stop() {
    if (stream_) stream_->Stop();
    if (!running_.exchange(false)) return;
    vsync_cv_.notify_all();
    impl_->readback_cv.notify_all();
    if (impl_->hwnd) PostMessageW(impl_->hwnd, WM_NULL, 0, 0);
    for (auto* t : {&vsync_thread_, &window_thread_, &readback_thread_})
        if (t->joinable()) t->join();
}

void DirectModeComponent::VsyncLoop() {
    using clock = std::chrono::steady_clock;
    const auto period_now = [this] {
        return std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(1.0 / refresh_hz_.load()));
    };
    PreciseSleeper sleeper;
    auto next = clock::now() + period_now();
    while (running_) {
        const auto period = period_now();
        sleeper.SleepUntil(next);
        vr::VRServerDriverHost()->VsyncEvent(0.0);
        {
            std::lock_guard<std::mutex> lock(vsync_mu_);
            ++vsync_count_;
        }
        vsync_cv_.notify_all();
        next += period;
        {   // a requested shift (display sync), at most half a millisecond per frame
            std::lock_guard<std::mutex> lock(vsync_mu_);
            const double step = std::clamp(pending_shift_s_, -0.0005, 0.0005);
            pending_shift_s_ -= step;
            next += std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(step));
        }
        const auto now = clock::now();
        if (now > next + period * 4) next = now + period; // fell far behind: resync, do not burst
    }
}

void DirectModeComponent::CreateSwapTextureSet(uint32_t pid, const SwapTextureSetDesc_t* desc, SwapTextureSet_t* out) {
    if (!desc || !out) return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = desc->nWidth;
    td.Height = desc->nHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT(desc->nFormat);
    td.SampleDesc.Count = desc->nSampleCount ? desc->nSampleCount : 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    std::lock_guard<std::mutex> lock(impl_->mu);
    vr::SharedTextureHandle_t first = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<IDXGIResource> res;
        HANDLE shared = nullptr;
        if (FAILED(impl_->device->CreateTexture2D(&td, nullptr, &tex)) || FAILED(tex.As(&res)) ||
            FAILED(res->GetSharedHandle(&shared))) {
            Log(LogLevel::Error, "direct mode: shared texture %u of %ux%u fmt %u failed", i, td.Width, td.Height, td.Format);
            std::lock_guard<std::mutex> s(mu_);
            stats_.last_error = "CreateSwapTextureSet failed";
            return;
        }
        const auto handle = reinterpret_cast<vr::SharedTextureHandle_t>(shared);
        if (i == 0) first = handle;
        impl_->textures[handle] = Impl::Tex{tex, pid, first, i};
        out->rSharedTextureHandles[i] = handle;
    }
    impl_->set_next[first] = 0;
    out->unTextureFlags = 0;
    std::lock_guard<std::mutex> s(mu_);
    ++stats_.swap_sets_created;
    ++stats_.swap_sets_alive;
    stats_.last_width = td.Width;
    stats_.last_height = td.Height;
    stats_.last_format = td.Format;
    Log(LogLevel::Info, "direct mode: swap texture set for pid %u: %ux%u DXGI format %u, %u samples", pid, td.Width,
        td.Height, td.Format, td.SampleDesc.Count);
}

void DirectModeComponent::DestroySwapTextureSet(vr::SharedTextureHandle_t handle) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    auto it = impl_->textures.find(handle);
    if (it == impl_->textures.end()) return;
    const auto first = it->second.first;
    for (auto t = impl_->textures.begin(); t != impl_->textures.end();) {
        t = t->second.first == first ? impl_->textures.erase(t) : std::next(t);
    }
    impl_->set_next.erase(first);
    std::lock_guard<std::mutex> s(mu_);
    if (stats_.swap_sets_alive) --stats_.swap_sets_alive;
}

void DirectModeComponent::DestroyAllSwapTextureSets(uint32_t pid) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    size_t sets = 0;
    for (auto t = impl_->textures.begin(); t != impl_->textures.end();) {
        if (t->second.pid == pid) {
            if (t->second.set_index == 0) {
                ++sets;
                impl_->set_next.erase(t->first);
            }
            t = impl_->textures.erase(t);
        } else {
            ++t;
        }
    }
    if (sets) Log(LogLevel::Info, "direct mode: released %zu swap set(s) of pid %u", sets, pid);
    std::lock_guard<std::mutex> s(mu_);
    stats_.swap_sets_alive -= std::min<uint64_t>(stats_.swap_sets_alive, sets);
}

void DirectModeComponent::GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2], uint32_t (*indices)[2]) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    vr::SharedTextureHandle_t advanced = 0;
    for (int eye = 0; eye < 2; ++eye) {
        auto it = impl_->textures.find(handles[eye]);
        if (it == impl_->textures.end()) continue;
        const auto first = it->second.first;
        uint32_t& next = impl_->set_next[first];
        if (first != advanced) { // both eyes may share one set: advance it once
            next = (next + 1) % 3;
            advanced = first;
        }
        if (indices) (*indices)[eye] = next;
    }
}

namespace {
// HmdMatrix34_t (rotation + translation) to x y z, qw qx qy qz.
bool PoseFromMatrix(const vr::HmdMatrix34_t& m, float out[7]) {
    const float (*r)[4] = m.m;
    const float tr = r[0][0] + r[1][1] + r[2][2];
    float w, x, y, z;
    if (tr > 0) {
        const float s = std::sqrt(tr + 1.0f) * 2;
        w = 0.25f * s; x = (r[2][1] - r[1][2]) / s; y = (r[0][2] - r[2][0]) / s; z = (r[1][0] - r[0][1]) / s;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2;
        w = (r[2][1] - r[1][2]) / s; x = 0.25f * s; y = (r[0][1] + r[1][0]) / s; z = (r[0][2] + r[2][0]) / s;
    } else if (r[1][1] > r[2][2]) {
        const float s = std::sqrt(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2;
        w = (r[0][2] - r[2][0]) / s; x = (r[0][1] + r[1][0]) / s; y = 0.25f * s; z = (r[1][2] + r[2][1]) / s;
    } else {
        const float s = std::sqrt(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2;
        w = (r[1][0] - r[0][1]) / s; x = (r[0][2] + r[2][0]) / s; y = (r[1][2] + r[2][1]) / s; z = 0.25f * s;
    }
    const float n = std::sqrt(w * w + x * x + y * y + z * z);
    if (!(n > 0.5f && n < 1.5f)) return false;   // not a rotation (e.g. all zero)
    const float v[7] = {r[0][3], r[1][3], r[2][3], w / n, x / n, y / n, z / n};
    std::memcpy(out, v, sizeof(v));
    return true;
}
} // namespace

void DirectModeComponent::SubmitLayer(const SubmitLayerPerEye_t (&per_eye)[2]) {
    std::lock_guard<std::mutex> lock(mu_);
    if (layer_count_ == 0) { // the first layer of a frame is the scene; overlays follow
        layer0_[0] = per_eye[0];
        layer0_[1] = per_eye[1];
    }
    ++layer_count_;
    ++stats_.layers_submitted;
}

void DirectModeComponent::Present(vr::SharedTextureHandle_t sync_texture) {
    SubmitLayerPerEye_t layer[2];
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++stats_.presents;
        have = layer_count_ > 0 && layer0_[0].hTexture && layer0_[1].hTexture;
        layer[0] = layer0_[0];
        layer[1] = layer0_[1];
        layer_count_ = 0;
        if (stats_.presents == 1) Log(LogLevel::Info, "direct mode: first frame presented by the compositor");
    }
    // The compositor guards its output with a keyed mutex on the sync texture.
    if (sync_texture && sync_texture != impl_->sync_handle) {
        impl_->sync_mutex.Reset();
        ComPtr<IDXGIKeyedMutex> km;
        if (SUCCEEDED(impl_->device->OpenSharedResource(reinterpret_cast<HANDLE>(sync_texture), IID_PPV_ARGS(&km)))) {
            impl_->sync_mutex = km;
        }
        impl_->sync_handle = sync_texture;
    }
    if (impl_->sync_mutex && impl_->sync_mutex->AcquireSync(0, 10) != S_OK) return;
    if (have) CopyPreview(layer);
    if (impl_->sync_mutex) impl_->sync_mutex->ReleaseSync(0);
}

void DirectModeComponent::CopyPreview(const SubmitLayerPerEye_t (&layer)[2]) {
    using namespace std::chrono;
    const auto now = steady_clock::now();
    // Frame-rate caps as deadlines rather than "a full period since the last
    // one": with presents at the cap rate, jitter made every other one a little
    // early and the preview ran at ~52 of 72 fps. A present up to a quarter period
    // early counts; the average stays at the cap.
    auto due = [&](steady_clock::time_point& next, double fps) {
        const auto period = duration_cast<steady_clock::duration>(duration<double>(1.0 / std::max(1.0, fps)));
        if (now + period / 4 < next) return false;
        next = std::max(next + period, now - period);   // after a pause, catch up at most one period
        return true;
    };
    const bool want_shm = preview_active_ && due(impl_->next_shm, capture_.fps);
    const bool want_win = window_visible_ && due(impl_->next_win, window_fps_);
    const bool want_stream = stream_ && stream_->Wanted();   // every frame, at the display rate
    if (!want_shm && !want_win && !want_stream) return; // nobody is looking: no GPU work at all

    std::unique_lock<std::mutex> lock(impl_->mu);
    auto l = impl_->textures.find(layer[0].hTexture), r = impl_->textures.find(layer[1].hTexture);
    if (l == impl_->textures.end() || r == impl_->textures.end()) return;
    D3D11_TEXTURE2D_DESC ed{};
    l->second.tex->GetDesc(&ed);
    if ((!IsRgba(ed.Format) && !IsBgra(ed.Format)) || ed.SampleDesc.Count != 1) {
        std::lock_guard<std::mutex> s(mu_);
        stats_.capture.state = "error: unsupported eye texture (format " + std::to_string(ed.Format) + ")";
        return;
    }
    const auto& b = layer[0].bounds;
    const uint32_t x0 = uint32_t(std::min(b.uMin, b.uMax) * float(ed.Width));
    const uint32_t x1 = uint32_t(std::max(b.uMin, b.uMax) * float(ed.Width));
    const uint32_t y0 = uint32_t(std::min(b.vMin, b.vMax) * float(ed.Height));
    const uint32_t y1 = uint32_t(std::max(b.vMin, b.vMax) * float(ed.Height));
    const uint32_t ew = std::max(1u, x1 - x0), eh = std::max(1u, y1 - y0);
    const DXGI_FORMAT fmt = Typed(ed.Format);

    if (want_stream) {
        // Both eyes into one UNORM texture (same bits: the values stay sRGB encoded)
        // for the encoder's video processor.
        const DXGI_FORMAT sf = IsBgra(ed.Format) ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!impl_->stream_src || impl_->stream_w != ew * 2 || impl_->stream_h != eh || impl_->stream_fmt != sf) {
            D3D11_TEXTURE2D_DESC st{};
            st.Width = ew * 2;
            st.Height = eh;
            st.MipLevels = 1;
            st.ArraySize = 1;
            st.Format = sf;
            st.SampleDesc.Count = 1;
            st.Usage = D3D11_USAGE_DEFAULT;
            st.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            impl_->stream_src.Reset();
            if (SUCCEEDED(impl_->device->CreateTexture2D(&st, nullptr, &impl_->stream_src))) {
                impl_->stream_w = ew * 2;
                impl_->stream_h = eh;
                impl_->stream_fmt = sf;
            }
        }
        if (impl_->stream_src) {
            const D3D11_BOX lbox{x0, y0, 0, x0 + ew, y0 + eh, 1};
            impl_->ctx->CopySubresourceRegion(impl_->stream_src.Get(), 0, 0, 0, 0, l->second.tex.Get(), 0, &lbox);
            const auto& sb = layer[1].bounds;
            D3D11_TEXTURE2D_DESC sd{};
            r->second.tex->GetDesc(&sd);
            const uint32_t sx0 = uint32_t(std::min(sb.uMin, sb.uMax) * float(sd.Width));
            const uint32_t sy0 = uint32_t(std::min(sb.vMin, sb.vMax) * float(sd.Height));
            const D3D11_BOX rbox{sx0, sy0, 0, std::min(sx0 + ew, sd.Width), std::min(sy0 + eh, sd.Height), 1};
            impl_->ctx->CopySubresourceRegion(impl_->stream_src.Get(), 0, ew, 0, 0, r->second.tex.Get(), 0, &rbox);
            float pose[7];
            const bool pose_ok = PoseFromMatrix(layer[0].mHmdPose, pose);
            stream_->Submit(impl_->stream_src.Get(), ew, eh, pose_ok ? pose : nullptr);
        }
    }
    if (!want_shm && !want_win) return;

    if (!impl_->atlas || impl_->atlas_w != ew * 2 || impl_->atlas_h != eh || impl_->atlas_format != fmt) {
        D3D11_TEXTURE2D_DESC ad{};
        ad.Width = ew * 2;
        ad.Height = eh;
        ad.MipLevels = 0; // full chain, filled by GenerateMips
        ad.ArraySize = 1;
        ad.Format = fmt;
        ad.SampleDesc.Count = 1;
        ad.Usage = D3D11_USAGE_DEFAULT;
        ad.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        ad.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
        impl_->atlas.Reset();
        impl_->atlas_srv.Reset();
        if (FAILED(impl_->device->CreateTexture2D(&ad, nullptr, &impl_->atlas)) ||
            FAILED(impl_->device->CreateShaderResourceView(impl_->atlas.Get(), nullptr, &impl_->atlas_srv))) {
            impl_->atlas.Reset();
            std::lock_guard<std::mutex> s(mu_);
            stats_.capture.state = "error: preview texture";
            return;
        }
        impl_->atlas_w = ew * 2;
        impl_->atlas_h = eh;
        impl_->atlas_format = fmt;
        auto level_for = [&](uint32_t max_w) {
            uint32_t lv = 0;
            while ((impl_->atlas_w >> lv) > max_w && (impl_->atlas_h >> lv) > 16) ++lv;
            return lv;
        };
        impl_->shm_level = level_for(std::max(32u, capture_.max_eye_width * 2));
        impl_->win_level = level_for(1920);
        for (auto& st : impl_->staging) st.Reset();
    }
    D3D11_BOX box{x0, y0, 0, x0 + ew, y0 + eh, 1};
    impl_->ctx->CopySubresourceRegion(impl_->atlas.Get(), 0, 0, 0, 0, l->second.tex.Get(), 0, &box);
    const auto& rb = layer[1].bounds;
    D3D11_TEXTURE2D_DESC rd{};
    r->second.tex->GetDesc(&rd);
    const uint32_t rx0 = uint32_t(std::min(rb.uMin, rb.uMax) * float(rd.Width));
    const uint32_t ry0 = uint32_t(std::min(rb.vMin, rb.vMax) * float(rd.Height));
    D3D11_BOX rbox{rx0, ry0, 0, std::min(rx0 + ew, rd.Width), std::min(ry0 + eh, rd.Height), 1};
    impl_->ctx->CopySubresourceRegion(impl_->atlas.Get(), 0, ew, 0, 0, r->second.tex.Get(), 0, &rbox);
    impl_->ctx->GenerateMips(impl_->atlas_srv.Get());

    if (want_shm) {
        const uint32_t w = std::max(1u, impl_->atlas_w >> impl_->shm_level);
        const uint32_t h = std::max(1u, impl_->atlas_h >> impl_->shm_level);
        const int idx = impl_->staging_pending == 0 ? 1 : 0;
        if (!impl_->staging[idx] || impl_->staging_w != w || impl_->staging_h != h) {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = w;
            sd.Height = h;
            sd.MipLevels = 1;
            sd.ArraySize = 1;
            sd.Format = fmt;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_STAGING;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& st : impl_->staging) {
                st.Reset();
                impl_->device->CreateTexture2D(&sd, nullptr, &st);
            }
            impl_->staging_w = w;
            impl_->staging_h = h;
        }
        if (impl_->staging[idx]) {
            impl_->ctx->CopySubresourceRegion(impl_->staging[idx].Get(), 0, 0, 0, 0, impl_->atlas.Get(), impl_->shm_level, nullptr);
            impl_->staging_pose_ok[idx] = PoseFromMatrix(layer[0].mHmdPose, impl_->staging_pose[idx]);
            impl_->staging_pending = idx;
            impl_->readback_cv.notify_one();
        }
        std::lock_guard<std::mutex> s(mu_);
        ++stats_.capture.submitted;
    }
    if (want_win) {
        impl_->window_frame_ready = true;
    }
}

void DirectModeComponent::PostPresent(const Throttling_t* /*throttling*/) {
    std::unique_lock<std::mutex> lock(vsync_mu_);
    const uint64_t start = vsync_count_;
    vsync_cv_.wait_for(lock, std::chrono::duration<double>(2.0 / refresh_hz_.load()),
                       [&] { return vsync_count_ != start || !running_; });
}

void DirectModeComponent::GetFrameTiming(vr::DriverDirectMode_FrameTiming* timing) {
    if (!timing || timing->m_nSize < sizeof(vr::DriverDirectMode_FrameTiming)) return;
    timing->m_nNumFramePresents = 1;
    timing->m_nNumMisPresented = 0;
    timing->m_nNumDroppedFrames = 0;
    timing->m_nReprojectionFlags = 0;
}

DirectModeStats DirectModeComponent::Stats() const {
    DirectModeStats s;
    {
        std::lock_guard<std::mutex> lock(mu_);
        s = stats_;
    }
    {
        std::lock_guard<std::mutex> lock(vsync_mu_);
        s.vsyncs = vsync_count_;
    }
    s.window_visible = window_visible_;
    s.window_frames = impl_->window_frames;
    if (stream_) s.stream = stream_->Stats();
    return s;
}

void DirectModeComponent::SetPreviewActive(bool active) {
    if (!capture_.enabled) return;
    preview_active_ = active;
    std::lock_guard<std::mutex> lock(mu_);
    if (!active) {
        stats_.capture.active = false;
        if (stats_.capture.state.rfind("error", 0) != 0) stats_.capture.state = "paused";
    } else if (stats_.capture.state == "paused") {
        stats_.capture.state = "waiting for first frame";
    }
}

void DirectModeComponent::SetWindowVisible(bool visible) {
    window_visible_ = visible;
    if (impl_->hwnd) PostMessageW(impl_->hwnd, WM_NULL, 0, 0);
}

namespace {
std::atomic<bool>* g_window_flag = nullptr;

LRESULT CALLBACK PreviewWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) { // closing hides the preview; SteamVR keeps running
        if (g_window_flag) *g_window_flag = false;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
} // namespace

void DirectModeComponent::WindowLoop() {
    g_window_flag = &window_visible_;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PreviewWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = L"VHMD_Preview";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"VHMD Preview", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 960,
                                560, nullptr, nullptr, wc.hInstance, nullptr);
    impl_->hwnd = hwnd;
    ComPtr<IDXGISwapChain1> swap;
    uint32_t sw = 0, sh = 0;
    DXGI_FORMAT sf = DXGI_FORMAT_UNKNOWN;
    bool shown = false, sized = false;
    while (running_ && hwnd) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        const bool visible = window_visible_;
        if (visible != shown) {
            ShowWindow(hwnd, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            shown = visible;
        }
        if (visible && impl_->window_frame_ready.exchange(false)) {
            std::unique_lock<std::mutex> lock(impl_->mu);
            if (impl_->atlas) {
                const uint32_t w = std::max(1u, impl_->atlas_w >> impl_->win_level);
                const uint32_t h = std::max(1u, impl_->atlas_h >> impl_->win_level);
                const DXGI_FORMAT f = SwapFormat(impl_->atlas_format);
                if (!swap || w != sw || h != sh || f != sf) {
                    swap.Reset();
                    DXGI_SWAP_CHAIN_DESC1 d{};
                    d.Width = w;
                    d.Height = h;
                    d.Format = f;
                    d.SampleDesc.Count = 1;
                    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                    d.BufferCount = 2;
                    d.Scaling = DXGI_SCALING_STRETCH;
                    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                    impl_->factory->CreateSwapChainForHwnd(impl_->device.Get(), hwnd, &d, nullptr, nullptr, &swap);
                    sw = w;
                    sh = h;
                    sf = f;
                    if (!sized) { // same aspect as the image, at most 960 px wide
                        const uint32_t cw = std::min(960u, w);
                        RECT rc{0, 0, LONG(cw), LONG(cw * h / std::max(1u, w))};
                        AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
                        SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
                        sized = true;
                    }
                }
                ComPtr<ID3D11Texture2D> back;
                if (swap && SUCCEEDED(swap->GetBuffer(0, IID_PPV_ARGS(&back)))) {
                    impl_->ctx->CopySubresourceRegion(back.Get(), 0, 0, 0, 0, impl_->atlas.Get(), impl_->win_level, nullptr);
                    back.Reset();
                    lock.unlock();
                    swap->Present(0, 0);
                    ++impl_->window_frames;
                }
            }
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 4, QS_ALLINPUT);
    }
    swap.Reset();
    if (hwnd) DestroyWindow(hwnd);
    impl_->hwnd = nullptr;
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

void DirectModeComponent::ReadbackLoop() {
    while (running_) {
        ComPtr<ID3D11Texture2D> tex;
        uint32_t w = 0, h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        float pose[7] = {};
        bool pose_ok = false;
        {
            std::unique_lock<std::mutex> lock(impl_->mu);
            impl_->readback_cv.wait_for(lock, std::chrono::milliseconds(200),
                                        [&] { return impl_->staging_pending >= 0 || !running_; });
            if (!running_ || impl_->staging_pending < 0) continue;
            tex = impl_->staging[impl_->staging_pending];
            std::memcpy(pose, impl_->staging_pose[impl_->staging_pending], sizeof(pose));
            pose_ok = impl_->staging_pose_ok[impl_->staging_pending];
            impl_->staging_pending = -1;
            w = impl_->staging_w;
            h = impl_->staging_h;
            fmt = impl_->atlas_format;
        }
        if (!tex) continue;
        // Poll instead of a blocking Map so the shared context lock is never held while the GPU works.
        D3D11_MAPPED_SUBRESOURCE m{};
        HRESULT hr = DXGI_ERROR_WAS_STILL_DRAWING;
        for (int i = 0; i < 200 && running_; ++i) {
            hr = impl_->ctx->Map(tex.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
            if (hr != DXGI_ERROR_WAS_STILL_DRAWING) break;
            Sleep(1);
        }
        if (FAILED(hr)) continue;
        const size_t stride = size_t(w) * 4, need = sizeof(PreviewHeader) + stride * h;
        if (impl_->view_size < need) {
            impl_->ReleaseShm();
            const std::wstring name = MappingName(capture_.path);
            impl_->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(uint64_t(need) >> 32),
                                                DWORD(need), name.c_str());
            if (impl_->mapping) impl_->view = static_cast<uint8_t*>(MapViewOfFile(impl_->mapping, FILE_MAP_ALL_ACCESS, 0, 0, need));
            if (!impl_->view) {
                impl_->ctx->Unmap(tex.Get(), 0);
                impl_->ReleaseShm();
                std::lock_guard<std::mutex> s(mu_);
                stats_.capture.state = "error: shared memory";
                continue;
            }
            impl_->view_size = need;
            std::memset(impl_->view, 0, sizeof(PreviewHeader));
        }
        auto* hdr = reinterpret_cast<PreviewHeader*>(impl_->view);
        uint8_t* px = impl_->view + sizeof(PreviewHeader);
        hdr->seq = hdr->seq | 1; // odd: writing
        MemoryBarrier();
        const bool bgra = IsBgra(fmt);
        for (uint32_t y = 0; y < h; ++y) {
            const uint8_t* src = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
            uint8_t* dst = px + size_t(y) * stride;
            if (!bgra) {
                std::memcpy(dst, src, stride);
            } else {
                for (uint32_t x = 0; x < w; ++x) {
                    dst[x * 4 + 0] = src[x * 4 + 2];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + 0];
                    dst[x * 4 + 3] = 255;
                }
            }
        }
        impl_->ctx->Unmap(tex.Get(), 0);
        std::memcpy(hdr->magic, kPreviewMagic, 8);
        hdr->version = kPreviewVersion;
        hdr->header_size = sizeof(PreviewHeader);
        hdr->width = w;
        hdr->height = h;
        hdr->stride = uint32_t(stride);
        hdr->format = kPreviewFormatRgba8;
        hdr->frame_index = ++impl_->frame_index;
        hdr->timestamp_ns = RealtimeNs();
        hdr->eye_width = w / 2;
        hdr->source_format = uint32_t(fmt);
        hdr->pose_flags = pose_ok ? 1u : 0u;
        std::memcpy(hdr->pose, pose, sizeof(pose));
        MemoryBarrier();
        hdr->seq = (hdr->seq | 1) + 1; // even: stable
        std::lock_guard<std::mutex> s(mu_);
        if (preview_active_) {
            stats_.capture.written = impl_->frame_index;
            stats_.capture.active = true;
            stats_.capture.state = "running";
        }
    }
}

} // namespace vhmd
