// Self-test of windows/stream_encoder without SteamVR: a D3D11 device renders a
// moving test pattern (both "eyes" side by side), the encoder streams it to a
// local TCP client, and the client checks the messages: one config, then
// access units with increasing frame indices, the pose, and a keyframe with
// parameter sets first. Writes the elementary stream next to the executable
// (stream_selftest.h264 / .hevc / .av1) for inspection with ffprobe.
//
//   vhmd_stream_selftest [h264|hevc|av1] [eye_width] [source_eye_width (1280)]
// Exit codes: 0 pass, 1 fail, 77 skipped (no hardware encoder).
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
#include <d3d11_4.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "windows/stream_encoder.h"

using Microsoft::WRL::ComPtr;
using namespace vhmd;

#pragma pack(push, 1)
struct Header {
    char magic[4];
    uint32_t kind, size, flags;
    uint64_t index, time_us;
    float pose[7];
    uint32_t reserved;
};
#pragma pack(pop)

static bool RecvAll(SOCKET s, void* p, size_t n) {
    auto* b = static_cast<char*>(p);
    while (n) {
        int r = recv(s, b, int(std::min<size_t>(n, 1 << 20)), 0);
        if (r <= 0) return false;
        b += r;
        n -= size_t(r);
    }
    return true;
}

int main(int argc, char** argv) {
    const std::string codec = argc > 1 ? argv[1] : "h264";
    const int eye_out = argc > 2 ? std::atoi(argv[2]) : 960;
    const int interval_us = argc > 4 ? std::atoi(argv[4]) : 13889;   // between frames
    const uint32_t eye_w = argc > 3 ? uint32_t(std::atoi(argv[3])) : 1280, eye_h = eye_w;   // source eye size

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &dev, nullptr, &ctx))) {
        std::printf("SKIP: no hardware D3D11 device\n");
        return 77;
    }
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(ctx.As(&mt))) mt->SetMultithreadProtected(TRUE);
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC ad{};
    dev.As(&dxgi);
    dxgi->GetAdapter(&adapter);
    adapter->GetDesc(&ad);
    const uint64_t luid = (uint64_t(uint32_t(ad.AdapterLuid.HighPart)) << 32) | ad.AdapterLuid.LowPart;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = eye_w * 2;
    td.Height = eye_h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> tex;
    dev->CreateTexture2D(&td, nullptr, &tex);
    std::vector<uint32_t> px(size_t(td.Width) * td.Height);

    StreamSettings ss;
    ss.port = 39593;
    StreamEncoder enc(dev.Get(), ctx.Get(), luid, ss);
    enc.SetRefreshRate(72);
    enc.Start();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET c = INVALID_SOCKET;
    for (int i = 0; i < 50 && c == INVALID_SOCKET; ++i) {
        c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(ss.port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(c, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            closesocket(c);
            c = INVALID_SOCKET;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
    if (c == INVALID_SOCKET) {
        std::printf("FAIL: cannot connect to the encoder\n");
        return 1;
    }
    const std::string settings = "{\"codec\":\"" + codec + "\",\"bitrate_mbps\":20,\"eye_width\":" +
                                 std::to_string(eye_out) + ",\"buffer\":1}\n";
    send(c, settings.data(), int(settings.size()), 0);

    std::atomic<bool> done{false};
    int configs = 0, frames = 0, posed = 0, keyframes_with_ps = 0;
    uint64_t last_index = 0, bytes = 0;
    uint64_t bytes_a = 0, bytes_b = 0;   // frames 72..215 at 20 Mbit/s, 288..431 after asking for 8
    bool ordered = true;
    std::string config;
    const std::string out_path = std::string("stream_selftest.") + codec;
    FILE* out = std::fopen(out_path.c_str(), "wb");
    std::thread reader([&] {
        Header h{};
        std::vector<uint8_t> payload;
        while (RecvAll(c, &h, sizeof(h))) {
            if (std::memcmp(h.magic, "VHV1", 4) != 0) break;
            payload.resize(h.size);
            if (h.size && !RecvAll(c, payload.data(), h.size)) break;
            if (h.kind == 1) {
                ++configs;
                config.assign(payload.begin(), payload.end());
            } else if (h.kind == 2) {
                ++frames;
                bytes += h.size;
                if (h.index >= 72 && h.index < 216) bytes_a += h.size;
                if (h.index >= 288 && h.index < 432) bytes_b += h.size;
                if (h.index && h.index <= last_index) ordered = false;
                if (h.index) last_index = h.index;
                if (h.flags & 1) ++posed;
                if (codec == "av1") {
                    // Low-overhead OBUs: header (type in bits 3-6, extension flag, size flag),
                    // leb128 size. A keyframe starts with a sequence header (type 1).
                    for (size_t i = 0; i < payload.size();) {
                        const uint8_t hb = payload[i];
                        if (!(hb & 2)) break;
                        size_t j = i + 1 + ((hb & 4) ? 1 : 0);
                        uint64_t sz = 0;
                        for (int shift = 0; j < payload.size(); shift += 7) {
                            sz |= uint64_t(payload[j] & 0x7F) << shift;
                            if (!(payload[j++] & 0x80)) break;
                        }
                        if (((hb >> 3) & 0xF) == 1) {
                            ++keyframes_with_ps;
                            break;
                        }
                        i = j + size_t(sz);
                    }
                }
                for (size_t i = 0; codec != "av1" && i + 4 < payload.size(); ++i)
                    if (payload[i] == 0 && payload[i + 1] == 0 && payload[i + 2] == 1) {
                        const int t = codec == "hevc" ? (payload[i + 3] >> 1) & 0x3F : payload[i + 3] & 0x1F;
                        if (t == (codec == "hevc" ? 32 : 7)) {
                            ++keyframes_with_ps;
                            break;
                        }
                    }
                if (out) std::fwrite(payload.data(), 1, payload.size(), out);
            }
        }
        done = true;
    });

    // 6 s at 72 fps: a vertical bar sweeping across, different colors per eye.
    // After 3 s the client asks for a lower bitrate while streaming.
    const auto t0 = std::chrono::steady_clock::now();
    const int total = 432;
    for (int n = 0; n < total; ++n) {
        if (n == 216) {
            const std::string lower = "{\"bitrate_mbps\":8}\n";
            send(c, lower.data(), int(lower.size()), 0);
        }
        const uint32_t bar = (n * 13) % eye_w;
        for (uint32_t y = 0; y < td.Height; ++y)
            for (uint32_t x = 0; x < td.Width; ++x) {
                const uint32_t ex = x % eye_w;
                const bool on = ex >= bar && ex < bar + 64;
                const uint8_t r = on ? 255 : uint8_t(ex * 255 / eye_w), g = uint8_t(y * 255 / td.Height),
                              b = x < eye_w ? 40 : 200;
                px[size_t(y) * td.Width + x] = 0xff000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
            }
        ctx->UpdateSubresource(tex.Get(), 0, nullptr, px.data(), td.Width * 4, 0);
        const float pose[7] = {0.1f * n, 1.6f, 0, 1, 0, 0, 0};
        enc.Submit(tex.Get(), eye_w, eye_h, pose);
        std::this_thread::sleep_until(t0 + std::chrono::microseconds(interval_us) * (n + 1));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const StreamStats st = enc.Stats();
    closesocket(c);
    reader.join();
    enc.Stop();
    if (out) std::fclose(out);

    std::printf("encoder: %s (%s), %ux%u\n", st.encoder.c_str(), st.state.c_str(), st.width, st.height);
    std::printf("submitted %llu, encoded %llu, dropped %llu\n", (unsigned long long)st.submitted,
                (unsigned long long)st.encoded, (unsigned long long)st.dropped);
    std::printf("timing: submit %.2f ms (max %.2f), submit to sent %.2f ms (max %.2f)\n", st.submit_ms_avg,
                st.submit_ms_max, st.encode_ms_avg, st.encode_ms_max);
    std::printf("timing: inside the encoder %.2f ms (max %.2f)\n", st.encoder_ms_avg, st.encoder_ms_max);
    std::printf("client: %d config, %d frames (%d with pose, %d with parameter sets), %.1f Mbit/s, ordered %s\n",
                configs, frames, posed, keyframes_with_ps, bytes * 8.0 / 6.0 / 1e6, ordered ? "yes" : "no");
    const double mbps_a = bytes_a * 8.0 / 2.0 / 1e6, mbps_b = bytes_b * 8.0 / 2.0 / 1e6;
    std::printf("bitrate: %.1f Mbit/s asked 20, then %.1f Mbit/s asked 8 while streaming\n", mbps_a, mbps_b);
    std::printf("config: %s\nstream written to %s\n", config.c_str(), out_path.c_str());
    if (st.state.rfind("error: no hardware", 0) == 0) {
        std::printf("SKIP: %s\n", st.state.c_str());
        return 77;
    }
    // The first frames go while the encoder starts (a few hundred ms).
    const bool pass = configs == 1 && frames >= total * 7 / 10 && posed >= frames - 2 && keyframes_with_ps >= 1 &&
                      ordered && config.find("\"buffer\":1") != std::string::npos && mbps_b < mbps_a * 0.7;
    std::printf("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
