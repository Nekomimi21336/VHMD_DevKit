// VHMD Link: a native OpenXR app for standalone headsets (Pico 4 Ultra first).
//
// Reads the head and controller poses and inputs every frame and sends them to
// the PC (`python -m vhmd_client.headset_bridge`) as VHMD protocol messages, one
// JSON array each, and shows VHMD's image (H.264 on TCP 39581).
//
// Finding the PC: an address in <external files dir>/pc_address.txt if present,
// else a "VHMD-DISCOVER" broadcast to UDP 39582 answered by the bridge; poses
// then go as UDP datagrams to port 39580 (Wi-Fi). With no answer the app falls
// back to USB: TCP to 127.0.0.1 (adb reverse), one array per line.
// The floor grid shows the link: green Wi-Fi, cyan USB, red none. Until the
// PC's image arrives, a panel in front of the user says what the app is doing
// (looking for the PC, waiting for SteamVR) and how to connect.
//
// Editions (CMake VHMD_DEV_EDITION): "VHMD Link" for using a PC's VR apps, and
// "VHMD Link Dev" which adds a statistics overlay and tells the PC (headset
// message "edition") to show controllers as colored axes.
//
// Coordinates: OpenXR's STAGE space (meters, +Y up, -Z forward, origin on the
// floor) matches SteamVR's standing space, so poses go out unchanged.

#include <android/log.h>
#include <android_native_app_glue.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <android/hardware_buffer.h>
#include <jni.h>
#include <aaudio/AAudio.h>
#include <android/configuration.h>
#include <media/NdkImageReader.h>
#include <media/NdkMediaCodec.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "text.h"

#define LOG_TAG "VHMDLink"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

constexpr uint16_t kPcPort = 39580;
constexpr uint16_t kDiscoveryPort = 39582;
enum Link { kNone = 0, kUsb = 1, kWifi = 2 };

// Where the PC is (network byte order) and how we reach it; set by the video
// thread, which looks for the PC, and read by the pose sender.
std::atomic<uint32_t> g_pc_addr{0};
std::atomic<int> g_link{kNone};
uint32_t g_manual_addr = 0;   // from pc_address.txt
std::mutex g_pc_mu;
std::string g_pc_name;        // the PC's name from its discovery reply

constexpr bool kDev = VHMD_DEV_EDITION != 0;
bool g_ja = false;            // system language is Japanese
const char* T(const char* ja, const char* en) { return g_ja ? ja : en; }

std::string Fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* f, ...) {
    char b[512];
    va_list ap;
    va_start(ap, f);
    vsnprintf(b, sizeof(b), f, ap);
    va_end(ap);
    return b;
}

// This headset's own IPv4 address on the LAN (for the connection screen).
std::string OwnAddress() {
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return "";
    std::string out;
    for (ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if (!(i->ifa_flags & IFF_UP)) continue;
        char b[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr, b, sizeof(b));
        out = b;
        if (strncmp(i->ifa_name, "wlan", 4) == 0) break;   // prefer Wi-Fi
    }
    freeifaddrs(list);
    return out;
}

// Reads "key":"value" from a small JSON text.
bool JsonString(const std::string& s, const char* key, std::string* out) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p = s.find_first_not_of(" \t", p + 1);
    if (p == std::string::npos || s[p] != '"') return false;
    const size_t e = s.find('"', p + 1);
    if (e == std::string::npos) return false;
    *out = s.substr(p + 1, e - p - 1);
    return true;
}

bool JsonBool(const std::string& s, const char* key, bool fallback) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return fallback;
    p = s.find(':', p + k.size());
    if (p == std::string::npos) return fallback;
    p = s.find_first_not_of(" \t", p + 1);
    return p != std::string::npos && s.compare(p, 4, "true") == 0;
}

std::string AddrText(uint32_t a) {
    char b[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &a, b, sizeof(b));
    return b;
}

// Broadcasts a discovery request; returns the answering PC's address or 0.
uint32_t Discover() {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    timeval tv{0, 700000};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in b{};
    b.sin_family = AF_INET;
    b.sin_port = htons(kDiscoveryPort);
    b.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    const char msg[] = "VHMD-DISCOVER 1";
    sendto(s, msg, sizeof(msg) - 1, 0, reinterpret_cast<sockaddr*>(&b), sizeof(b));
    char buf[512];
    sockaddr_in from{};
    socklen_t fl = sizeof(from);
    const ssize_t n = recvfrom(s, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fl);
    close(s);
    if (n <= 0) return 0;
    buf[n] = 0;
    if (!strstr(buf, "\"vhmd\"")) return 0;
    std::string name;
    if (JsonString(buf, "name", &name)) {
        std::lock_guard<std::mutex> l(g_pc_mu);
        g_pc_name = name;
    }
    return from.sin_addr.s_addr;
}

bool Check(XrResult r, const char* what) {
    if (XR_FAILED(r)) {
        LOGE("%s failed: %d", what, static_cast<int>(r));
        return false;
    }
    return true;
}

// -- network ------------------------------------------------------------------
// The render loop hands over the newest message; a thread (re)connects and
// sends it, so a missing PC never stalls a frame. Older unsent messages are
// dropped: only the latest pose matters.
class Sender {
public:
    void Start() { thread_ = std::thread([this] { Run(); }); }
    void Stop() {
        {
            std::lock_guard<std::mutex> l(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void Post(std::string line) {
        {
            std::lock_guard<std::mutex> l(mu_);
            pending_ = std::move(line);
            has_ = true;
        }
        cv_.notify_one();
    }
    bool Connected() const { return connected_.load(); }

    // Messages the PC sent back (e.g. {"type":"haptic",...}), oldest first.
    std::vector<std::string> TakeIncoming() {
        std::lock_guard<std::mutex> l(in_mu_);
        std::vector<std::string> out;
        out.swap(incoming_);
        return out;
    }

private:
    void Keep(std::string msg) {
        std::lock_guard<std::mutex> l(in_mu_);
        if (incoming_.size() < 64) incoming_.push_back(std::move(msg));
    }

    // Non-blocking reads of what came back on either link.
    void Receive(int fd, int udp) {
        char buf[2048];
        if (udp >= 0) {
            for (;;) {
                const ssize_t n = recv(udp, buf, sizeof(buf), MSG_DONTWAIT);
                if (n <= 0) break;
                Keep(std::string(buf, static_cast<size_t>(n)));
            }
        }
        if (fd >= 0) {
            for (;;) {
                const ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
                if (n <= 0) break;
                tcp_in_.append(buf, static_cast<size_t>(n));
            }
            for (size_t nl; (nl = tcp_in_.find('\n')) != std::string::npos;) {
                Keep(tcp_in_.substr(0, nl));
                tcp_in_.erase(0, nl + 1);
            }
        }
    }

    void Run() {
        int fd = -1;    // USB: TCP to 127.0.0.1 (adb reverse)
        int udp = -1;   // Wi-Fi: datagrams to the PC
        while (true) {
            std::string line;
            {
                std::unique_lock<std::mutex> l(mu_);
                cv_.wait_for(l, std::chrono::milliseconds(500), [this] { return stop_ || has_; });
                if (stop_) break;
                if (!has_) continue;
                line.swap(pending_);
                has_ = false;
            }
            const int link = g_link.load();
            if (link == kWifi) {
                if (fd >= 0) {
                    close(fd);
                    fd = -1;
                }
                if (udp < 0) udp = socket(AF_INET, SOCK_DGRAM, 0);
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_port = htons(kPcPort);
                a.sin_addr.s_addr = g_pc_addr.load();
                // One array per datagram, without the line break.
                const size_t n = line.size() - (line.empty() || line.back() != '\n' ? 0 : 1);
                connected_ = sendto(udp, line.data(), n, 0, reinterpret_cast<sockaddr*>(&a), sizeof(a)) > 0;
                Receive(-1, udp);
                continue;
            }
            if (link != kUsb) {
                connected_ = false;
                continue;
            }
            if (fd < 0) fd = Connect();
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            const char* p = line.data();
            size_t left = line.size();
            while (left > 0) {
                ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
                if (n <= 0) break;
                p += n;
                left -= static_cast<size_t>(n);
            }
            if (left > 0) {
                close(fd);
                fd = -1;
                connected_ = false;
                LOGI("PC connection lost");
            } else {
                Receive(fd, -1);
            }
        }
        if (fd >= 0) close(fd);
        if (udp >= 0) close(udp);
    }

    int Connect() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(kPcPort);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // adb reverse
        if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
            close(fd);
            return -1;
        }
        connected_ = true;
        LOGI("connected to the PC");
        return fd;
    }

    std::thread thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::string pending_;
    bool has_ = false;
    bool stop_ = false;
    std::atomic<bool> connected_{false};
    std::mutex in_mu_;
    std::vector<std::string> incoming_;
    std::string tcp_in_;
};

// -- small math ---------------------------------------------------------------
struct Mat4 {
    float m[16];   // column-major
};

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

Mat4 FromPose(const XrPosef& p) {
    const float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    Mat4 r{};
    r.m[0] = 1 - 2 * (y * y + z * z);
    r.m[1] = 2 * (x * y + z * w);
    r.m[2] = 2 * (x * z - y * w);
    r.m[4] = 2 * (x * y - z * w);
    r.m[5] = 1 - 2 * (x * x + z * z);
    r.m[6] = 2 * (y * z + x * w);
    r.m[8] = 2 * (x * z + y * w);
    r.m[9] = 2 * (y * z - x * w);
    r.m[10] = 1 - 2 * (x * x + y * y);
    r.m[12] = p.position.x;
    r.m[13] = p.position.y;
    r.m[14] = p.position.z;
    r.m[15] = 1;
    return r;
}

Mat4 InvertRigid(const Mat4& a) {   // rotation + translation only
    Mat4 r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[j * 4 + i] = a.m[i * 4 + j];
    for (int i = 0; i < 3; ++i)
        r.m[12 + i] = -(r.m[i] * a.m[12] + r.m[4 + i] * a.m[13] + r.m[8 + i] * a.m[14]);
    r.m[15] = 1;
    return r;
}

Mat4 Projection(const XrFovf& fov, float n, float f) {
    const float l = std::tan(fov.angleLeft), r = std::tan(fov.angleRight);
    const float u = std::tan(fov.angleUp), d = std::tan(fov.angleDown);
    const float w = r - l, h = u - d;
    Mat4 m{};
    m.m[0] = 2 / w;
    m.m[5] = 2 / h;
    m.m[8] = (r + l) / w;
    m.m[9] = (u + d) / h;
    m.m[10] = -(f + n) / (f - n);
    m.m[11] = -1;
    m.m[14] = -(2 * f * n) / (f - n);
    return m;
}

XrQuaternionf QuatMul(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

XrVector3f Rotate(const XrQuaternionf& q, const XrVector3f& v) {
    const XrQuaternionf p{v.x, v.y, v.z, 0};
    const XrQuaternionf c{-q.x, -q.y, -q.z, q.w};
    const XrQuaternionf r = QuatMul(QuatMul(q, p), c);
    return {r.x, r.y, r.z};
}

// a then b: b given in a's frame.
XrPosef Compose(const XrPosef& a, const XrPosef& b) {
    const XrVector3f t = Rotate(a.orientation, b.position);
    return {QuatMul(a.orientation, b.orientation), {a.position.x + t.x, a.position.y + t.y, a.position.z + t.z}};
}

// -- GL ---------------------------------------------------------------------
const char* kVs = R"(#version 300 es
uniform mat4 mvp;
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 col;
out vec3 vcol;
void main() { vcol = col; gl_Position = mvp * vec4(pos, 1.0); }
)";
const char* kFs = R"(#version 300 es
precision mediump float;
in vec3 vcol;
out vec4 o;
void main() { o = vec4(vcol, 1.0); }
)";

GLuint Compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        LOGE("shader: %s", log);
    }
    return s;
}

struct Vertex {
    float x, y, z, r, g, b;
};

void Line(std::vector<Vertex>& v, float x0, float y0, float z0, float x1, float y1, float z1, float r, float g,
          float b) {
    v.push_back({x0, y0, z0, r, g, b});
    v.push_back({x1, y1, z1, r, g, b});
}

void Triad(std::vector<Vertex>& v, const XrPosef& p, float len) {
    const Mat4 m = FromPose(p);
    const float o[3] = {m.m[12], m.m[13], m.m[14]};
    const float cols[3][3] = {{1, 0.2f, 0.2f}, {0.2f, 1, 0.2f}, {0.3f, 0.5f, 1}};
    for (int a = 0; a < 3; ++a)
        Line(v, o[0], o[1], o[2], o[0] + m.m[a * 4] * len, o[1] + m.m[a * 4 + 1] * len, o[2] + m.m[a * 4 + 2] * len,
             cols[a][0], cols[a][1], cols[a][2]);
}

// -- audio ------------------------------------------------------------------
// The PC's sound (kind 3 PCM, kind 4 format) played with AAudio. The network
// thread writes into a single-producer / single-consumer ring; the AAudio
// callback reads it. Playback starts once ~40 ms are buffered (and again after
// running dry); more than ~150 ms buffered means we fell behind, so the newest
// data is skipped until the backlog drains.
class AudioOut {
public:
    ~AudioOut() { Close(); }

    void Configure(int rate, int channels) {
        if (stream_ && rate == rate_ && channels == channels_) return;
        Close();
        rate_ = rate;
        channels_ = channels;
        ring_.assign(static_cast<size_t>(rate) * channels, 0);   // 1 s
        head_ = tail_ = 0;
        primed_ = false;
        AAudioStreamBuilder* b = nullptr;
        if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
        AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(b, channels);
        AAudioStreamBuilder_setSampleRate(b, rate);
        AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setDataCallback(b, &AudioOut::Callback, this);
        const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &stream_);
        AAudioStreamBuilder_delete(b);
        if (r != AAUDIO_OK) {
            LOGE("audio: cannot open an output stream (%d)", static_cast<int>(r));
            stream_ = nullptr;
            return;
        }
        AAudioStream_requestStart(stream_);
        LOGI("audio: playing %d Hz, %d channel(s)", rate, channels);
    }

    void Push(const int16_t* s, size_t n) {
        if (!stream_ || ring_.empty()) return;
        const size_t cap = ring_.size();
        const size_t fill = head_.load() - tail_.load();
        if (fill + n > cap || fill > static_cast<size_t>(rate_) * channels_ * 150 / 1000) {
            ++skipped_;
            return;
        }
        size_t h = head_.load();
        for (size_t i = 0; i < n; ++i) ring_[(h + i) % cap] = s[i];
        head_.store(h + n);
        ++packets_;
    }

    void TakeStats(int* packets, int* underruns, int* skipped) {
        *packets = packets_.exchange(0);
        *underruns = underruns_.exchange(0);
        *skipped = skipped_.exchange(0);
    }

private:
    static aaudio_data_callback_result_t Callback(AAudioStream*, void* user, void* data, int32_t frames) {
        auto* self = static_cast<AudioOut*>(user);
        auto* out = static_cast<int16_t*>(data);
        const size_t want = static_cast<size_t>(frames) * self->channels_;
        const size_t cap = self->ring_.size();
        size_t t = self->tail_.load();
        const size_t fill = self->head_.load() - t;
        const size_t start_level = static_cast<size_t>(self->rate_) * self->channels_ * 40 / 1000;
        if (!self->primed_ && fill >= start_level) self->primed_ = true;
        if (!self->primed_ || fill < want) {
            std::memset(out, 0, want * sizeof(int16_t));
            if (self->primed_) {   // ran dry: wait for the buffer to fill up again
                self->primed_ = false;
                ++self->underruns_;
            }
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        for (size_t i = 0; i < want; ++i) out[i] = self->ring_[(t + i) % cap];
        self->tail_.store(t + want);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    void Close() {
        if (stream_) {
            AAudioStream_requestStop(stream_);
            AAudioStream_close(stream_);
            stream_ = nullptr;
        }
    }

    AAudioStream* stream_ = nullptr;
    int rate_ = 0, channels_ = 0;
    std::vector<int16_t> ring_;
    std::atomic<size_t> head_{0}, tail_{0};
    bool primed_ = false;   // callback thread only
    std::atomic<int> packets_{0}, underruns_{0}, skipped_{0};
};

// -- video ------------------------------------------------------------------
// VHMD's eye images arrive as H.264 access units on TCP 39581 (adb reverse;
// clients/python/vhmd_client/headset_stream.py). A network thread feeds them to
// the hardware decoder (MediaCodec), which renders into an AImageReader; the
// render thread binds the newest image as an external texture straight from its
// AHardwareBuffer (no copy) and draws each eye's half of it.
constexpr uint16_t kVideoPort = 39581;
constexpr uint32_t kKindConfig = 1, kKindFrame = 2, kKindAudio = 3, kKindAudioFormat = 4;
constexpr uint64_t kPtsStepUs = 11111;   // decoder timestamps: frame index * this

#pragma pack(push, 1)
struct VideoHeader {
    char magic[4];
    uint32_t kind;
    uint32_t size;
    uint32_t flags;
    uint64_t frame_index;
    uint64_t pc_time_us;
    float pose[7];
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(VideoHeader) == 64, "video header");

// Reads a number after "key": in a small JSON text (enough for the config).
bool JsonNumber(const std::string& s, const char* key, double* out) {
    const std::string k = std::string("\"") + key + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) return false;
    p = s.find(':', p);
    if (p == std::string::npos) return false;
    *out = strtod(s.c_str() + p + 1, nullptr);
    return true;
}

PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC pEglGetNativeClientBuffer = nullptr;
PFNEGLCREATEIMAGEKHRPROC pEglCreateImage = nullptr;
PFNEGLDESTROYIMAGEKHRPROC pEglDestroyImage = nullptr;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pGlEglImageTargetTexture2D = nullptr;

class Video {
public:
    void Start() {
        thread_ = std::thread([this] { Run(); });
        drain_thread_ = std::thread([this] { DrainLoop(); });
    }
    void Stop() {
        stop_ = true;
        int fd = fd_.exchange(-1);
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
        if (drain_thread_.joinable()) drain_thread_.join();
        if (codec_) {
            AMediaCodec_stop(codec_);
            AMediaCodec_delete(codec_);
        }
    }

    // Render thread: binds the newest decoded frame. True while a frame is shown.
    bool Update(EGLDisplay display) {
        AImageReader* reader = reader_.load();
        if (!reader) return has_frame_;
        if (!tex_) {
            glGenTextures(1, &tex_);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        // Decoded images wait in a queue. buffer 0: always the newest (lowest
        // latency; when one arrives just after a display frame, that frame shows
        // the old image again and the next one skips an image). buffer 1: one image
        // is held back, so a late arrival is covered by the one in reserve, at the
        // cost of about one frame of latency.
        // Images are taken from the reader as soon as the decoder renders them
        // (OnImage, on the reader's thread): left in the reader, a newer one
        // replaces an older one before the render loop gets to it.
        const auto now = std::chrono::steady_clock::now();
        AImage* img = nullptr;
        {
            std::lock_guard<std::mutex> l(queue_mu_);
            const size_t keep = static_cast<size_t>(buffer_.load()) + 1;
            while (queue_.size() > keep) {   // more than wanted: drop the oldest
                AImage_delete(queue_.front().img);
                queue_.pop_front();
                ++dropped_;
            }
            if (!queue_.empty()) {
                img = queue_.front().img;
                const float w = std::chrono::duration<float, std::milli>(now - queue_.front().arrived).count();
                waits_ms_.push_back(w);
                // The wait as a phase of the display period, for the PC's vsync
                // sync (a wait of almost a period = arrived just too late).
                if (period_ms_ > 1) {
                    const float a = 6.2831853f * std::fmod(w, period_ms_) / period_ms_;
                    phase_c_ += std::cos(a);
                    phase_s_ += std::sin(a);
                    ++phase_n_;
                }
                queue_.pop_front();
            }
        }
        if (!img) {
            // No new image for a second: the PC stopped sending (SteamVR closed,
            // link lost); show the app's own scene and the connection panel.
            if (has_frame_ && now - last_new_ > std::chrono::seconds(1)) {
                has_frame_ = false;
                LOGI("video: no new image for 1 s");
            }
            if (has_frame_) ++repeats_;
            return has_frame_;
        }
        AHardwareBuffer* hb = nullptr;
        AImage_getHardwareBuffer(img, &hb);
        EGLImageKHR egl = EGL_NO_IMAGE_KHR;
        if (hb) {
            const EGLint attrs[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
            egl = pEglCreateImage(display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, pEglGetNativeClientBuffer(hb),
                                  attrs);
        }
        if (egl == EGL_NO_IMAGE_KHR) {
            AImage_delete(img);
            return has_frame_;
        }
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex_);
        pGlEglImageTargetTexture2D(GL_TEXTURE_EXTERNAL_OES, egl);
        // The previous image can go back to the decoder now.
        if (cur_egl_ != EGL_NO_IMAGE_KHR) pEglDestroyImage(display, cur_egl_);
        if (cur_img_) AImage_delete(cur_img_);
        cur_img_ = img;
        cur_egl_ = egl;
        has_frame_ = true;
        last_new_ = now;
        // Which frame this is: the decoder keeps our timestamp (frame index * step).
        int64_t ts_ns = 0;
        AImage_getTimestamp(img, &ts_ns);
        const uint64_t index = static_cast<uint64_t>((ts_ns / 1000 + kPtsStepUs / 2) / kPtsStepUs);
        {
            std::lock_guard<std::mutex> l(pose_mu_);
            const FrameRec& f = poses_[index % kPoseRing];
            cur_pose_valid_ = f.valid && f.index == index;
            cur_pose_ = f.pose;
        }
        ++shown_;
        return true;
    }

    GLuint texture() const { return tex_; }
    void SetPeriodMs(float ms) { period_ms_ = ms; }
    // Frame wait since the last call as a circular mean over the display period
    // (slack_ms in [0, period)), how consistent it was (0..1) and how many frames.
    bool TakeTiming(float* slack_ms, float* weight, int* n) {
        std::lock_guard<std::mutex> l(queue_mu_);
        *n = phase_n_;
        const bool ok = phase_n_ > 0 && period_ms_ > 1;
        if (ok) {
            float a = std::atan2(phase_s_, phase_c_);
            if (a < 0) a += 6.2831853f;
            *slack_ms = a / 6.2831853f * period_ms_;
            *weight = std::sqrt(phase_c_ * phase_c_ + phase_s_ * phase_s_) / phase_n_;
        }
        phase_c_ = phase_s_ = 0;
        phase_n_ = 0;
        return ok;
    }
    // Head pose the shown frame was rendered for (render thread).
    bool FramePose(XrPosef* out) const {
        if (!cur_pose_valid_) return false;
        *out = cur_pose_;
        return true;
    }
    bool HasFov() const { return has_fov_.load(); }
    XrFovf Fov(int eye) const { return fov_[eye]; }
    bool Connected() const { return fd_.load() >= 0; }
    struct Stats {
        int received, decoded, shown, repeats, dropped, buffer, rx_close, dec_close;
        int audio_packets, audio_underruns, audio_skipped;
        float wait_avg_ms, wait_p95_ms;   // decoded image waiting before it is shown
        long long bytes;                  // received on the video connection
        float decode_avg_ms, decode_max_ms;   // frame to decoder -> image out
    };
    Stats TakeStats() {
        std::lock_guard<std::mutex> l(queue_mu_);
        Stats s{received_.exchange(0), decoded_.exchange(0), shown_, repeats_, dropped_, buffer_.load(),
                rx_close_.exchange(0), dec_close_.exchange(0), 0, 0, 0, 0, 0, bytes_.exchange(0)};
        audio_.TakeStats(&s.audio_packets, &s.audio_underruns, &s.audio_skipped);
        s.decode_avg_ms = s.decode_max_ms = 0;
        if (!decode_ms_.empty()) {
            float sum = 0;
            for (float d : decode_ms_) {
                sum += d;
                s.decode_max_ms = std::max(s.decode_max_ms, d);
            }
            s.decode_avg_ms = sum / decode_ms_.size();
            decode_ms_.clear();
        }
        if (!waits_ms_.empty()) {
            float sum = 0;
            for (float w : waits_ms_) sum += w;
            s.wait_avg_ms = sum / waits_ms_.size();
            std::sort(waits_ms_.begin(), waits_ms_.end());
            s.wait_p95_ms = waits_ms_[std::min(waits_ms_.size() - 1, waits_ms_.size() * 95 / 100)];
        }
        shown_ = repeats_ = dropped_ = 0;
        waits_ms_.clear();
        return s;
    }

private:
    void Run() {
        uint32_t last_wifi = 0;
        while (!stop_) {
            // Wi-Fi first (a fixed address, else discovery), then USB.
            uint32_t addr = g_manual_addr ? g_manual_addr : Discover();
            if (addr) {
                if (addr != last_wifi) LOGI("PC found on the network: %s", AddrText(addr).c_str());
                last_wifi = addr;
                g_pc_addr = addr;
                g_link = kWifi;
            } else {
                addr = htonl(INADDR_LOOPBACK);
                g_pc_addr = addr;
                g_link = kUsb;
            }
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(kVideoPort);
            a.sin_addr.s_addr = addr;
            if (fd < 0 || connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
                if (fd >= 0) close(fd);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            }
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
            int rcv = 4 << 20;
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
            fd_ = fd;
            LOGI("video: connected over %s (%s)", g_link.load() == kWifi ? "Wi-Fi" : "USB", AddrText(addr).c_str());
            std::vector<uint8_t> payload;
            VideoHeader h{};
            while (!stop_ && ReadAll(fd, &h, sizeof(h)) && memcmp(h.magic, "VHV1", 4) == 0) {
                payload.resize(h.size);
                if (h.size && !ReadAll(fd, payload.data(), h.size)) break;
                bytes_ += sizeof(h) + h.size;
                if (h.kind == kKindConfig)
                    Configure(std::string(payload.begin(), payload.end()));
                else if (h.kind == kKindFrame && codec_) {
                    RememberPose(h);
                    NoteGap(last_rx_, rx_close_);
                    Feed(payload, h.frame_index);
                } else if (h.kind == kKindAudioFormat) {
                    const std::string j(payload.begin(), payload.end());
                    double rate = 48000, channels = 2;
                    JsonNumber(j, "rate", &rate);
                    JsonNumber(j, "channels", &channels);
                    audio_.Configure(static_cast<int>(rate), static_cast<int>(channels));
                } else if (h.kind == kKindAudio) {
                    audio_.Push(reinterpret_cast<const int16_t*>(payload.data()), payload.size() / 2);
                }
            }
            LOGI("video: disconnected");
            int old = fd_.exchange(-1);
            if (old >= 0) close(old);
        }
    }

    // Render pose of each frame, looked up when its decoded image is shown.
    struct FrameRec {
        uint64_t index = 0;
        bool valid = false;
        XrPosef pose{};
    };
    static constexpr int kPoseRing = 64;

    void RememberPose(const VideoHeader& h) {
        std::lock_guard<std::mutex> l(pose_mu_);
        FrameRec& f = poses_[h.frame_index % kPoseRing];
        f.index = h.frame_index;
        f.valid = (h.flags & 1) != 0;
        // x y z, qw qx qy qz
        f.pose.position = {h.pose[0], h.pose[1], h.pose[2]};
        f.pose.orientation = {h.pose[4], h.pose[5], h.pose[6], h.pose[3]};
    }

    // Arrivals closer than a third of a frame (two at once): counts bunching.
    static void NoteGap(std::chrono::steady_clock::time_point& last, std::atomic<int>& close) {
        const auto t = std::chrono::steady_clock::now();
        if (t - last < std::chrono::milliseconds(4)) ++close;
        last = t;
    }

    // Reader thread: a decoded image is ready.
    void OnImage(AImageReader* r) {
        AImage* in = nullptr;
        if (AImageReader_acquireNextImage(r, &in) != AMEDIA_OK || !in) return;
        NoteGap(last_dec_, dec_close_);
        const auto now = std::chrono::steady_clock::now();
        int64_t ts_ns = 0;
        AImage_getTimestamp(in, &ts_ns);
        const uint64_t index = static_cast<uint64_t>((ts_ns / 1000 + kPtsStepUs / 2) / kPtsStepUs);
        std::lock_guard<std::mutex> l(queue_mu_);
        // Decode time: from handing the frame to the decoder until its image is here.
        if (fed_index_[index % kPoseRing] == index)
            decode_ms_.push_back(std::chrono::duration<float, std::milli>(now - fed_at_[index % kPoseRing]).count());
        queue_.push_back({in, now});
        while (queue_.size() > 4) {   // the render loop is not taking them: keep the newest
            AImage_delete(queue_.front().img);
            queue_.pop_front();
            ++dropped_;
        }
    }

    static bool ReadAll(int fd, void* dst, size_t n) {
        auto* p = static_cast<uint8_t*>(dst);
        while (n > 0) {
            ssize_t r = recv(fd, p, n, 0);
            if (r <= 0) return false;
            p += r;
            n -= static_cast<size_t>(r);
        }
        return true;
    }

    void Configure(const std::string& json) {
        double w = 0, h = 0, buffer = 0;
        JsonNumber(json, "width", &w);
        JsonNumber(json, "height", &h);
        if (JsonNumber(json, "buffer", &buffer)) buffer_ = std::clamp(static_cast<int>(buffer), 0, 3);
        // "fov": [[l, r, u, d], [l, r, u, d]] in radians (OpenXR angles).
        size_t p = json.find("\"fov\"");
        if (p != std::string::npos && json.find('[', p) != std::string::npos) {
            const char* c = json.c_str() + json.find('[', p) + 1;
            float v[8];
            int n = 0;
            while (n < 8 && *c) {
                if ((*c >= '0' && *c <= '9') || *c == '-' || *c == '.') {
                    char* end = nullptr;
                    v[n++] = strtof(c, &end);
                    c = end;
                } else {
                    ++c;
                }
            }
            if (n == 8) {
                for (int e = 0; e < 2; ++e) fov_[e] = {v[e * 4], v[e * 4 + 1], v[e * 4 + 2], v[e * 4 + 3]};
                has_fov_ = true;
            }
        }
        const std::string mime = json.find("\"codec\":\"hevc\"") != std::string::npos  ? "video/hevc"
                                 : json.find("\"codec\":\"av1\"") != std::string::npos ? "video/av01"
                                                                                         : "video/avc";
        LOGI("video: config %s %.0fx%.0f, fov %s, buffer %d", mime.c_str(), w, h, has_fov_ ? "from the PC" : "own",
             buffer_.load());
        if (codec_ && w == width_ && h == height_ && mime == mime_) return;   // same stream: keep the decoder
        std::lock_guard<std::mutex> cl(codec_mu_);   // the drain thread uses codec_
        if (codec_) {
            AMediaCodec_stop(codec_);
            AMediaCodec_delete(codec_);
            codec_ = nullptr;
        }
        width_ = static_cast<int>(w);
        height_ = static_cast<int>(h);
        AImageReader* reader = reader_.load();
        if (!reader) {
            if (AImageReader_newWithUsage(width_, height_, AIMAGE_FORMAT_PRIVATE,
                                          AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 8, &reader) != AMEDIA_OK) {
                LOGE("video: AImageReader failed");
                return;
            }
            AImageReader_ImageListener listener{this, [](void* ctx, AImageReader* r) {
                                                    static_cast<Video*>(ctx)->OnImage(r);
                                                }};
            AImageReader_setImageListener(reader, &listener);
            reader_ = reader;
        }
        ANativeWindow* window = nullptr;
        AImageReader_getWindow(reader, &window);
        mime_ = mime;
        codec_ = AMediaCodec_createDecoderByType(mime.c_str());
        if (!codec_) {
            LOGE("video: no %s decoder", mime.c_str());
            return;
        }
        AMediaFormat* f = AMediaFormat_new();
        AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime.c_str());
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, width_);
        AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, height_);
        AMediaFormat_setInt32(f, "low-latency", 1);
        AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
        // Output in decode order (no B-frames are sent): the decoder does not hold a
        // frame back waiting for reordering that never happens.
        AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
        AMediaFormat_setInt32(f, "priority", 0);
        media_status_t st = AMediaCodec_configure(codec_, f, window, nullptr, 0);
        AMediaFormat_delete(f);
        if (st != AMEDIA_OK || AMediaCodec_start(codec_) != AMEDIA_OK) {
            LOGE("video: decoder start failed (%d)", static_cast<int>(st));
            AMediaCodec_delete(codec_);
            codec_ = nullptr;
            return;
        }
        LOGI("video: %s decoder started", mime_.c_str());
    }

    void Feed(const std::vector<uint8_t>& au, uint64_t index) {
        ssize_t i = AMediaCodec_dequeueInputBuffer(codec_, 20000);
        if (i < 0) return;   // decoder full: drop this frame
        size_t cap = 0;
        uint8_t* buf = AMediaCodec_getInputBuffer(codec_, static_cast<size_t>(i), &cap);
        const size_t n = std::min(cap, au.size());
        memcpy(buf, au.data(), n);
        {
            std::lock_guard<std::mutex> l(queue_mu_);
            fed_at_[index % kPoseRing] = std::chrono::steady_clock::now();
            fed_index_[index % kPoseRing] = index;
        }
        AMediaCodec_queueInputBuffer(codec_, static_cast<size_t>(i), 0, n, index * kPtsStepUs, 0);
        ++received_;
    }

    // Own thread: each decoded frame goes to the reader the moment it is ready.
    // (Draining right after feeding the next frame held every late frame back by
    // one, and frames came out in pairs.) MediaCodec allows input and output on
    // different threads; codec_mu_ only guards against Configure replacing it.
    void DrainLoop() {
        while (!stop_) {
            std::unique_lock<std::mutex> cl(codec_mu_);
            if (!codec_) {
                cl.unlock();
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            AMediaCodecBufferInfo info;
            const ssize_t o = AMediaCodec_dequeueOutputBuffer(codec_, &info, 2000);
            if (o >= 0) {
                AMediaCodec_releaseOutputBuffer(codec_, static_cast<size_t>(o), true);
                ++decoded_;
            }
        }
    }

    std::thread thread_, drain_thread_;
    AudioOut audio_;
    std::mutex codec_mu_;
    std::atomic<bool> stop_{false};
    std::atomic<int> fd_{-1};
    std::atomic<AImageReader*> reader_{nullptr};
    AMediaCodec* codec_ = nullptr;
    int width_ = 0, height_ = 0;
    std::string mime_;
    XrFovf fov_[2] = {};
    std::atomic<bool> has_fov_{false};
    std::atomic<int> received_{0}, decoded_{0};
    std::chrono::steady_clock::time_point last_rx_{}, last_dec_{};
    std::atomic<int> rx_close_{0}, dec_close_{0};
    std::atomic<long long> bytes_{0};
    int shown_ = 0;
    std::mutex pose_mu_;
    FrameRec poses_[kPoseRing];
    bool cur_pose_valid_ = false;
    XrPosef cur_pose_{};
    std::atomic<int> buffer_{0};   // images held back (config "buffer")
    struct Queued {
        AImage* img;
        std::chrono::steady_clock::time_point arrived;
    };
    std::mutex queue_mu_;
    std::deque<Queued> queue_;   // filled by OnImage, emptied by Update
    int repeats_ = 0, dropped_ = 0;
    std::vector<float> waits_ms_;
    std::vector<float> decode_ms_;
    std::chrono::steady_clock::time_point fed_at_[64] = {};
    uint64_t fed_index_[64] = {};
    float period_ms_ = 0;                  // the headset's display period
    float phase_c_ = 0, phase_s_ = 0;      // sums of cos / sin of the wait phase
    int phase_n_ = 0;
    // render thread only
    GLuint tex_ = 0;
    AImage* cur_img_ = nullptr;
    EGLImageKHR cur_egl_ = EGL_NO_IMAGE_KHR;
    bool has_frame_ = false;
    std::chrono::steady_clock::time_point last_new_{};
};

const char* kVideoVs = R"(#version 300 es
uniform vec4 uv_rect;   // u0, v0, u1, v1 of this eye in the side-by-side frame
out vec2 uv;
void main() {
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));   // triangle strip corners
    uv = vec2(mix(uv_rect.x, uv_rect.z, p.x), mix(uv_rect.w, uv_rect.y, p.y));   // image row 0 at the top
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";
const char* kVideoFs = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES tex;
in vec2 uv;
out vec4 o;
// The stream is sRGB encoded and the swapchain re-encodes on write: linearize.
vec3 linear(vec3 c) { return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c)); }
void main() { o = vec4(linear(texture(tex, uv).rgb), 1.0); }
)";

// -- app ----------------------------------------------------------------------
struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    GLuint fbo = 0, depth = 0;
};

enum Hand { kLeft = 0, kRight = 1 };

// A flat panel of text shown as an OpenXR quad layer; redrawn only when its
// text changes.
struct Panel {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t width = 0, height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    bool has_image = false;
    std::string key;            // text currently shown
    XrSpace space = XR_NULL_HANDLE;
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
    XrExtent2Df size{1, 1};     // meters
    bool visible = false;
};

using vhmd_link::TextLine;

struct App {
    android_app* android = nullptr;
    bool resumed = false;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    bool exit = false;
    XrSpace base = XR_NULL_HANDLE;   // stage (floor) space
    XrSpace head = XR_NULL_HANDLE;   // view space
    std::vector<XrViewConfigurationView> view_configs;
    std::vector<Swapchain> swapchains;
    bool has_bd_controller = false;
    bool has_refresh_rate = false;
    std::string system_name;
    std::chrono::steady_clock::time_point next_info{};
    int reprojected = 0;   // frames shown at their render pose (stats)
    int haptics = 0;       // vibrations applied (stats)
    // X + A held: take control back to "manual" (the person wearing the headset).
    std::chrono::steady_clock::time_point take_since{};
    bool take_holding = false, take_sent = false;
    std::string link_stats;   // {"type":"link_stats",...} for the PC, refreshed every 2 s

    // What the PC says about itself ({"type":"status",...} about once a second).
    struct PcStatus {
        bool known = false;
        bool steamvr = false, driver = false;
        std::string control;    // who moves the HMD: "manual", "auto", another sender, "" = anyone
        double refresh = 0;     // display refresh rate the PC wants (0 = the headset's own)
        std::chrono::steady_clock::time_point at{};
    } pc;
    std::string shown_control;  // last owner announced in a notice
    std::vector<float> refresh_rates;   // offered by the headset (XR_FB_display_refresh_rate)
    float requested_refresh = 0;
    std::chrono::steady_clock::time_point next_timing{};

    // Panels: the connection screen (follows the gaze lazily), a notice line
    // and (Dev edition) the statistics overlay.
    int64_t color_format = 0;
    vhmd_link::TextPainter text;
    bool text_ok = false;
    Panel status_panel, notice_panel, hud_panel;
    bool status_placed = false, status_moving = false;
    std::string notice;
    std::chrono::steady_clock::time_point notice_until{};
    std::string own_address;
    std::chrono::steady_clock::time_point next_address{};
    // Last 2-second statistics, for the overlay.
    float st_display = 0, st_received = 0, st_shown = 0, st_wait = 0, st_mbps = 0, st_audio = 0;
    int st_underruns = 0, st_haptics = 0;

    XrActionSet action_set = XR_NULL_HANDLE;
    XrPath hand_paths[2] = {};
    XrAction grip_pose = XR_NULL_HANDLE, trigger = XR_NULL_HANDLE, trigger_touch = XR_NULL_HANDLE,
             squeeze = XR_NULL_HANDLE, stick = XR_NULL_HANDLE, stick_click = XR_NULL_HANDLE,
             stick_touch = XR_NULL_HANDLE, btn_a = XR_NULL_HANDLE, btn_b = XR_NULL_HANDLE, btn_x = XR_NULL_HANDLE,
             btn_y = XR_NULL_HANDLE, menu = XR_NULL_HANDLE, haptic = XR_NULL_HANDLE;
    XrSpace grip_spaces[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    GLuint program = 0, vbo = 0, vao = 0;
    GLint mvp_loc = -1;
    GLuint video_prog = 0, empty_vao = 0;
    GLint uv_rect_loc = -1;

    Sender sender;
    Video video;
};

void OnAppCmd(android_app* a, int32_t cmd) {
    App* app = static_cast<App*>(a->userData);
    switch (cmd) {
    case APP_CMD_RESUME: app->resumed = true; break;
    case APP_CMD_PAUSE: app->resumed = false; break;
    case APP_CMD_DESTROY: app->exit = true; break;
    default: break;
    }
}

bool InitLoader(App& app) {
    PFN_xrInitializeLoaderKHR init = nullptr;
    if (!Check(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                     reinterpret_cast<PFN_xrVoidFunction*>(&init)),
               "xrGetInstanceProcAddr(xrInitializeLoaderKHR)"))
        return false;
    XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    li.applicationVM = app.android->activity->vm;
    li.applicationContext = app.android->activity->clazz;
    return Check(init(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&li)), "xrInitializeLoaderKHR");
}

bool CreateInstance(App& app) {
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> props(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, props.data());
    std::vector<const char*> exts = {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
                                     XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME};
    for (auto& p : props) {
        if (strcmp(p.extensionName, XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME) == 0) {
            exts.push_back(XR_BD_CONTROLLER_INTERACTION_EXTENSION_NAME);
            app.has_bd_controller = true;
        }
        if (strcmp(p.extensionName, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME) == 0) {
            exts.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
            app.has_refresh_rate = true;
        }
    }
    LOGI("%u runtime extensions; Pico controller profile: %s", n, app.has_bd_controller ? "yes" : "no");

    XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    android_info.applicationVM = app.android->activity->vm;
    android_info.applicationActivity = app.android->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &android_info;
    strncpy(ci.applicationInfo.applicationName, "VHMD Link", XR_MAX_APPLICATION_NAME_SIZE - 1);
    ci.applicationInfo.applicationVersion = 1;
    strncpy(ci.applicationInfo.engineName, "none", XR_MAX_ENGINE_NAME_SIZE - 1);
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    ci.enabledExtensionCount = static_cast<uint32_t>(exts.size());
    ci.enabledExtensionNames = exts.data();
    if (!Check(xrCreateInstance(&ci, &app.instance), "xrCreateInstance")) return false;

    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(app.instance, &ip);
    LOGI("runtime: %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion),
         XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo si{XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Check(xrGetSystem(app.instance, &si, &app.system), "xrGetSystem")) return false;
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(app.instance, app.system, &sp);
    app.system_name = sp.systemName;
    return true;
}

bool InitEgl(App& app) {
    app.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(app.display, nullptr, nullptr);
    const EGLint attrs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                            EGL_RENDERABLE_TYPE, 0x0040 /* EGL_OPENGL_ES3_BIT */, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(app.display, attrs, &app.config, 1, &count) || count == 0) {
        LOGE("eglChooseConfig failed");
        return false;
    }
    const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    app.context = eglCreateContext(app.display, app.config, EGL_NO_CONTEXT, ctx_attrs);
    const EGLint pb[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    app.surface = eglCreatePbufferSurface(app.display, app.config, pb);
    if (app.context == EGL_NO_CONTEXT || !eglMakeCurrent(app.display, app.surface, app.surface, app.context)) {
        LOGE("EGL context failed");
        return false;
    }
    return true;
}

bool CreateSession(App& app) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR reqs_fn = nullptr;
    xrGetInstanceProcAddr(app.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&reqs_fn));
    XrGraphicsRequirementsOpenGLESKHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!reqs_fn || !Check(reqs_fn(app.instance, app.system, &reqs), "xrGetOpenGLESGraphicsRequirementsKHR"))
        return false;

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = app.display;
    binding.config = app.config;
    binding.context = app.context;
    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = app.system;
    if (!Check(xrCreateSession(app.instance, &ci, &app.session), "xrCreateSession")) return false;

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.poseInReferenceSpace.orientation.w = 1;
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    if (XR_FAILED(xrCreateReferenceSpace(app.session, &rs, &app.base))) {
        LOGI("no STAGE space; using LOCAL");
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        if (!Check(xrCreateReferenceSpace(app.session, &rs, &app.base), "xrCreateReferenceSpace(LOCAL)")) return false;
    }
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!Check(xrCreateReferenceSpace(app.session, &rs, &app.head), "xrCreateReferenceSpace(VIEW)")) return false;

    // The highest refresh rate the headset offers (Pico 4 Ultra: 72 / 90 Hz).
    if (app.has_refresh_rate) {
        PFN_xrEnumerateDisplayRefreshRatesFB enum_fn = nullptr;
        PFN_xrRequestDisplayRefreshRateFB request_fn = nullptr;
        xrGetInstanceProcAddr(app.instance, "xrEnumerateDisplayRefreshRatesFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&enum_fn));
        xrGetInstanceProcAddr(app.instance, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&request_fn));
        uint32_t n = 0;
        if (enum_fn && request_fn && XR_SUCCEEDED(enum_fn(app.session, 0, &n, nullptr)) && n > 0) {
            app.refresh_rates.resize(n);
            enum_fn(app.session, n, &n, app.refresh_rates.data());
            std::string list;
            for (float r : app.refresh_rates) list += Fmt("%.0f ", r);
            LOGI("refresh rates offered: %sHz (the PC chooses)", list.c_str());
        }
    }
    return true;
}

bool CreateSwapchains(App& app) {
    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(app.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n,
                                      nullptr);
    app.view_configs.assign(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(app.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n,
                                      app.view_configs.data());

    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(app.session, 0, &fc, nullptr);
    std::vector<int64_t> formats(fc);
    xrEnumerateSwapchainFormats(app.session, fc, &fc, formats.data());
    int64_t format = formats.empty() ? GL_RGBA8 : formats[0];
    for (int64_t f : formats)
        if (f == GL_SRGB8_ALPHA8) format = f;
    std::string list;
    for (int64_t f : formats) list += std::to_string(f) + " ";
    LOGI("swapchain formats: %s-> 0x%llx", list.c_str(), static_cast<long long>(format));
    app.color_format = format;

    for (auto& vc : app.view_configs) {
        Swapchain sc;
        sc.width = static_cast<int32_t>(vc.recommendedImageRectWidth);
        sc.height = static_cast<int32_t>(vc.recommendedImageRectHeight);
        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
        ci.format = format;
        ci.sampleCount = 1;
        ci.width = vc.recommendedImageRectWidth;
        ci.height = vc.recommendedImageRectHeight;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;
        if (!Check(xrCreateSwapchain(app.session, &ci, &sc.handle), "xrCreateSwapchain")) return false;
        uint32_t ic = 0;
        xrEnumerateSwapchainImages(sc.handle, 0, &ic, nullptr);
        sc.images.assign(ic, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        xrEnumerateSwapchainImages(sc.handle, ic, &ic, reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data()));
        glGenFramebuffers(1, &sc.fbo);
        glGenRenderbuffers(1, &sc.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, sc.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, sc.width, sc.height);
        app.swapchains.push_back(sc);
        LOGI("eye swapchain %dx%d, %u images", sc.width, sc.height, ic);
    }
    return true;
}

XrPath Path(App& app, const char* s) {
    XrPath p = XR_NULL_PATH;
    xrStringToPath(app.instance, s, &p);
    return p;
}

XrAction MakeAction(App& app, const char* name, XrActionType type, bool both_hands) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(ci.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    if (both_hands) {
        ci.countSubactionPaths = 2;
        ci.subactionPaths = app.hand_paths;
    }
    XrAction a = XR_NULL_HANDLE;
    Check(xrCreateAction(app.action_set, &ci, &a), name);
    return a;
}

void SuggestBindings(App& app, const char* profile, bool has_menu_left, bool has_ab_xy) {
    struct B {
        XrAction a;
        const char* path;
    };
    std::vector<B> b = {
        {app.grip_pose, "/user/hand/left/input/grip/pose"},   {app.grip_pose, "/user/hand/right/input/grip/pose"},
        {app.trigger, "/user/hand/left/input/trigger/value"}, {app.trigger, "/user/hand/right/input/trigger/value"},
        {app.trigger_touch, "/user/hand/left/input/trigger/touch"},
        {app.trigger_touch, "/user/hand/right/input/trigger/touch"},
        {app.squeeze, "/user/hand/left/input/squeeze/value"}, {app.squeeze, "/user/hand/right/input/squeeze/value"},
        {app.stick, "/user/hand/left/input/thumbstick"},      {app.stick, "/user/hand/right/input/thumbstick"},
        {app.stick_click, "/user/hand/left/input/thumbstick/click"},
        {app.stick_click, "/user/hand/right/input/thumbstick/click"},
        {app.stick_touch, "/user/hand/left/input/thumbstick/touch"},
        {app.stick_touch, "/user/hand/right/input/thumbstick/touch"},
    };
    if (has_ab_xy) {
        b.push_back({app.btn_x, "/user/hand/left/input/x/click"});
        b.push_back({app.btn_y, "/user/hand/left/input/y/click"});
        b.push_back({app.btn_a, "/user/hand/right/input/a/click"});
        b.push_back({app.btn_b, "/user/hand/right/input/b/click"});
    }
    if (has_menu_left) b.push_back({app.menu, "/user/hand/left/input/menu/click"});
    b.push_back({app.haptic, "/user/hand/left/output/haptic"});
    b.push_back({app.haptic, "/user/hand/right/output/haptic"});
    std::vector<XrActionSuggestedBinding> sb;
    for (auto& x : b) sb.push_back({x.a, Path(app, x.path)});
    XrInteractionProfileSuggestedBinding s{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    s.interactionProfile = Path(app, profile);
    s.suggestedBindings = sb.data();
    s.countSuggestedBindings = static_cast<uint32_t>(sb.size());
    XrResult r = xrSuggestInteractionProfileBindings(app.instance, &s);
    LOGI("bindings for %s: %s", profile, XR_SUCCEEDED(r) ? "ok" : "not accepted");
}

bool CreateActions(App& app) {
    XrActionSetCreateInfo ci{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(ci.actionSetName, "vhmd");
    strcpy(ci.localizedActionSetName, "VHMD");
    if (!Check(xrCreateActionSet(app.instance, &ci, &app.action_set), "xrCreateActionSet")) return false;
    app.hand_paths[kLeft] = Path(app, "/user/hand/left");
    app.hand_paths[kRight] = Path(app, "/user/hand/right");

    app.grip_pose = MakeAction(app, "grip_pose", XR_ACTION_TYPE_POSE_INPUT, true);
    app.trigger = MakeAction(app, "trigger", XR_ACTION_TYPE_FLOAT_INPUT, true);
    app.trigger_touch = MakeAction(app, "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    app.squeeze = MakeAction(app, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT, true);
    app.stick = MakeAction(app, "thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT, true);
    app.stick_click = MakeAction(app, "thumbstick_click", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    app.stick_touch = MakeAction(app, "thumbstick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT, true);
    app.btn_a = MakeAction(app, "a", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    app.btn_b = MakeAction(app, "b", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    app.btn_x = MakeAction(app, "x", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    app.btn_y = MakeAction(app, "y", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    app.menu = MakeAction(app, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT, false);
    app.haptic = MakeAction(app, "haptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, true);

    if (app.has_bd_controller) SuggestBindings(app, "/interaction_profiles/bytedance/pico4_controller", true, true);
    SuggestBindings(app, "/interaction_profiles/oculus/touch_controller", true, true);

    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sc.action = app.grip_pose;
        sc.subactionPath = app.hand_paths[h];
        sc.poseInActionSpace.orientation.w = 1;
        Check(xrCreateActionSpace(app.session, &sc, &app.grip_spaces[h]), "xrCreateActionSpace");
    }
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &app.action_set;
    return Check(xrAttachSessionActionSets(app.session, &ai), "xrAttachSessionActionSets");
}

void InitGl(App& app) {
    app.program = glCreateProgram();
    glAttachShader(app.program, Compile(GL_VERTEX_SHADER, kVs));
    glAttachShader(app.program, Compile(GL_FRAGMENT_SHADER, kFs));
    glLinkProgram(app.program);
    app.mvp_loc = glGetUniformLocation(app.program, "mvp");
    glGenVertexArrays(1, &app.vao);
    glGenBuffers(1, &app.vbo);
    glBindVertexArray(app.vao);
    glBindBuffer(GL_ARRAY_BUFFER, app.vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), reinterpret_cast<void*>(3 * sizeof(float)));
    glBindVertexArray(0);

    app.video_prog = glCreateProgram();
    glAttachShader(app.video_prog, Compile(GL_VERTEX_SHADER, kVideoVs));
    glAttachShader(app.video_prog, Compile(GL_FRAGMENT_SHADER, kVideoFs));
    glLinkProgram(app.video_prog);
    app.uv_rect_loc = glGetUniformLocation(app.video_prog, "uv_rect");
    glGenVertexArrays(1, &app.empty_vao);
    pEglGetNativeClientBuffer = reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(
        eglGetProcAddress("eglGetNativeClientBufferANDROID"));
    pEglCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pEglDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pGlEglImageTargetTexture2D = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
}

// -- per frame ----------------------------------------------------------------
float GetFloat(App& app, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
    xrGetActionStateFloat(app.session, &gi, &s);
    return s.isActive ? s.currentState : 0.0f;
}

bool GetBool(App& app, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
    xrGetActionStateBoolean(app.session, &gi, &s);
    return s.isActive && s.currentState;
}

XrVector2f GetVec2(App& app, XrAction a, XrPath sub) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = a;
    gi.subactionPath = sub;
    XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
    xrGetActionStateVector2f(app.session, &gi, &s);
    return s.isActive ? s.currentState : XrVector2f{0, 0};
}

void AppendPose(std::string& out, const char* id, const XrSpaceLocation& loc) {
    char buf[256];
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    if ((loc.locationFlags & need) != need) {
        snprintf(buf, sizeof(buf), "{\"type\":\"pose\",\"id\":\"%s\",\"valid\":false,\"src\":\"manual\"}", id);
    } else {
        const XrPosef& p = loc.pose;
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"pose\",\"id\":\"%s\",\"pos\":[%.5f,%.5f,%.5f],\"rot\":[%.6f,%.6f,%.6f,%.6f],"
                 "\"src\":\"manual\"}",
                 id,
                 p.position.x, p.position.y, p.position.z, p.orientation.w, p.orientation.x, p.orientation.y,
                 p.orientation.z);
    }
    if (out.size() > 1) out += ',';
    out += buf;
}

void AppendInputs(App& app, std::string& out, int hand) {
    const XrPath sub = app.hand_paths[hand];
    const float trig = GetFloat(app, app.trigger, sub);
    const float grip = GetFloat(app, app.squeeze, sub);
    const XrVector2f st = GetVec2(app, app.stick, sub);
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"input\",\"id\":\"%s\",\"src\":\"manual\",\"values\":{\"trigger\":%.3f,\"trigger_click\":%s,"
                     "\"trigger_touch\":%s,\"grip\":%.3f,\"grip_click\":%s,\"joystick_x\":%.3f,\"joystick_y\":%.3f,"
                     "\"joystick_click\":%s,\"joystick_touch\":%s",
                     hand == kLeft ? "left_hand" : "right_hand", trig, trig >= 0.95f ? "true" : "false",
                     GetBool(app, app.trigger_touch, sub) || trig > 0.05f ? "true" : "false", grip,
                     grip >= 0.9f ? "true" : "false", st.x, st.y, GetBool(app, app.stick_click, sub) ? "true" : "false",
                     GetBool(app, app.stick_touch, sub) ? "true" : "false");
    if (hand == kLeft)
        snprintf(buf + n, sizeof(buf) - n, ",\"x\":%s,\"y\":%s,\"menu\":%s}}",
                 GetBool(app, app.btn_x, XR_NULL_PATH) ? "true" : "false",
                 GetBool(app, app.btn_y, XR_NULL_PATH) ? "true" : "false",
                 GetBool(app, app.menu, XR_NULL_PATH) ? "true" : "false");
    else
        snprintf(buf + n, sizeof(buf) - n, ",\"a\":%s,\"b\":%s}}", GetBool(app, app.btn_a, XR_NULL_PATH) ? "true" : "false",
                 GetBool(app, app.btn_b, XR_NULL_PATH) ? "true" : "false");
    out += ',';
    out += buf;
}

// {"type":"headset",...}: this headset's display, so the PC can make VHMD render
// for it (vhmd_client.headset_info). eye_local: eye poses in head space.
void AppendHeadsetInfo(App& app, std::string& out, const std::vector<XrView>& eye_local) {
    float refresh = 0;
    if (app.has_refresh_rate) {
        PFN_xrGetDisplayRefreshRateFB get = nullptr;
        xrGetInstanceProcAddr(app.instance, "xrGetDisplayRefreshRateFB", reinterpret_cast<PFN_xrVoidFunction*>(&get));
        if (get) get(app.session, &refresh);
    }
    char buf[1024];
    int n = snprintf(buf, sizeof(buf), "{\"type\":\"headset\",\"model\":\"%s\",\"refresh\":%.1f,\"render\":[%u,%u],\"fov\":[",
                     app.system_name.c_str(), refresh, app.view_configs[0].recommendedImageRectWidth,
                     app.view_configs[0].recommendedImageRectHeight);
    for (size_t i = 0; i < eye_local.size() && i < 2; ++i) {
        const XrFovf& f = eye_local[i].fov;
        n += snprintf(buf + n, sizeof(buf) - n, "%s[%.5f,%.5f,%.5f,%.5f]", i ? "," : "", f.angleLeft, f.angleRight,
                      f.angleUp, f.angleDown);
    }
    n += snprintf(buf + n, sizeof(buf) - n, "],\"eyes\":[");
    for (size_t i = 0; i < eye_local.size() && i < 2; ++i) {
        const XrPosef& p = eye_local[i].pose;
        n += snprintf(buf + n, sizeof(buf) - n, "%s[%.5f,%.5f,%.5f,%.6f,%.6f,%.6f,%.6f]", i ? "," : "", p.position.x,
                      p.position.y, p.position.z, p.orientation.w, p.orientation.x, p.orientation.y, p.orientation.z);
    }
    snprintf(buf + n, sizeof(buf) - n, "],\"edition\":\"%s\",\"version\":\"%s\"}", kDev ? "dev" : "link",
             VHMD_LINK_VERSION);
    if (out.size() > 1) out += ',';
    out += buf;
}

// -- Wi-Fi --------------------------------------------------------------------
// A low-latency Wi-Fi lock (Android 10+, WIFI_MODE_FULL_LOW_LATENCY): without
// it the headset's Wi-Fi saves power between bursts and video frames arrive in
// bunches. Held while the app runs (Android drops it when the app is not in front).
jobject AcquireWifiLock(App& app) {
    JNIEnv* env = nullptr;
    JavaVM* vm = app.android->activity->vm;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return nullptr;
    jobject activity = app.android->activity->clazz;
    jclass ctx_cls = env->GetObjectClass(activity);
    jmethodID get_service = env->GetMethodID(ctx_cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring name = env->NewStringUTF("wifi");
    jobject wifi = get_service ? env->CallObjectMethod(activity, get_service, name) : nullptr;
    env->DeleteLocalRef(name);
    env->DeleteLocalRef(ctx_cls);
    jobject lock = nullptr;
    if (wifi && !env->ExceptionCheck()) {
        jclass wm_cls = env->GetObjectClass(wifi);
        jmethodID create = env->GetMethodID(wm_cls, "createWifiLock", "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;");
        jstring tag = env->NewStringUTF("VHMDLink");
        jobject l = create ? env->CallObjectMethod(wifi, create, 4 /* WIFI_MODE_FULL_LOW_LATENCY */, tag) : nullptr;
        env->DeleteLocalRef(tag);
        if (l && !env->ExceptionCheck()) {
            jclass lock_cls = env->GetObjectClass(l);
            jmethodID acquire = env->GetMethodID(lock_cls, "acquire", "()V");
            if (acquire) env->CallVoidMethod(l, acquire);
            if (!env->ExceptionCheck()) lock = env->NewGlobalRef(l);
            env->DeleteLocalRef(lock_cls);
        }
        if (l) env->DeleteLocalRef(l);
        env->DeleteLocalRef(wm_cls);
    }
    if (wifi) env->DeleteLocalRef(wifi);
    if (env->ExceptionCheck()) env->ExceptionClear();
    LOGI("Wi-Fi low-latency lock: %s", lock ? "held" : "not available");
    return lock;
}

void ReleaseWifiLock(App& app, jobject lock) {
    if (!lock) return;
    JNIEnv* env = nullptr;
    if (app.android->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK || !env) return;
    jclass cls = env->GetObjectClass(lock);
    jmethodID release = env->GetMethodID(cls, "release", "()V");
    if (release) env->CallVoidMethod(lock, release);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteGlobalRef(lock);
}

// -- panels -------------------------------------------------------------------
bool CreatePanel(App& app, Panel& p, int32_t w, int32_t h, XrSpace space, XrExtent2Df size) {
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = app.color_format;
    ci.sampleCount = 1;
    ci.width = static_cast<uint32_t>(w);
    ci.height = static_cast<uint32_t>(h);
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    if (!Check(xrCreateSwapchain(app.session, &ci, &p.handle), "xrCreateSwapchain(panel)")) return false;
    uint32_t ic = 0;
    xrEnumerateSwapchainImages(p.handle, 0, &ic, nullptr);
    p.images.assign(ic, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(p.handle, ic, &ic, reinterpret_cast<XrSwapchainImageBaseHeader*>(p.images.data()));
    p.width = w;
    p.height = h;
    p.space = space;
    p.size = size;
    return true;
}

// Shows `lines` on the panel (painted again only when they change).
void SetPanel(App& app, Panel& p, uint32_t background, float margin, const std::vector<TextLine>& lines) {
    p.visible = true;
    if (!app.text_ok || p.handle == XR_NULL_HANDLE) {
        p.visible = false;
        return;
    }
    std::string key;
    for (const auto& l : lines) key += l.text + '\n';
    if (p.has_image && key == p.key) return;
    std::vector<uint8_t> rgba;
    if (!app.text.Paint(p.width, p.height, background, margin, lines, &rgba)) {
        p.visible = p.has_image;
        return;
    }
    uint32_t image = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(p.handle, &ai, &image))) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(p.handle, &wi);
    glBindTexture(GL_TEXTURE_2D, p.images[image].image);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, p.width, p.height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(p.handle, &ri);
    p.has_image = true;
    p.key = key;
}

void AddPanelLayer(const Panel& p, std::vector<XrCompositionLayerQuad>& quads) {
    if (!p.visible || !p.has_image) return;
    XrCompositionLayerQuad q{XR_TYPE_COMPOSITION_LAYER_QUAD};
    q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    q.space = p.space;
    q.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    q.subImage.swapchain = p.handle;
    q.subImage.imageRect = {{0, 0}, {p.width, p.height}};
    q.pose = p.pose;
    q.size = p.size;
    quads.push_back(q);
}

void CreatePanels(App& app) {
    app.text_ok = app.text.Init(app.android->activity->vm);
    if (!app.text_ok) {
        LOGE("text drawing not available: no panels");
        return;
    }
    CreatePanel(app, app.status_panel, 1024, 640, app.base, {0.96f, 0.6f});
    CreatePanel(app, app.notice_panel, 1024, 112, app.head, {0.62f, 0.068f});
    app.notice_panel.pose = {{0, 0, 0, 1}, {0, -0.16f, -1.0f}};
    if (kDev) {
        CreatePanel(app, app.hud_panel, 1024, 150, app.head, {0.6f, 0.088f});
        // Below the line of sight, turned up toward the eyes.
        const float a = -std::atan(0.3f / 1.0f);
        app.hud_panel.pose = {{std::sin(a / 2), 0, 0, std::cos(a / 2)}, {0, -0.3f, -1.0f}};
    }
}

// The connection screen stays where it is until the user looks more than 35
// degrees away, then glides back in front of them (1.4 m away, facing them).
void PlaceStatusPanel(App& app, const XrSpaceLocation& head) {
    if (!(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return;
    const XrVector3f f = Rotate(head.pose.orientation, {0, 0, -1});
    float fx = f.x, fz = f.z;
    const float len = std::sqrt(fx * fx + fz * fz);
    if (len < 0.2f) return;   // looking straight up or down: keep it where it is
    fx /= len;
    fz /= len;
    const XrVector3f hp = head.pose.position;
    const XrVector3f target{hp.x + fx * 1.4f, hp.y - 0.05f, hp.z + fz * 1.4f};
    Panel& p = app.status_panel;
    if (!app.status_placed) {
        p.pose.position = target;
        app.status_placed = true;
    } else {
        const float dx = p.pose.position.x - hp.x, dz = p.pose.position.z - hp.z;
        const float dl = std::sqrt(dx * dx + dz * dz);
        const float cosang = dl > 1e-3f ? (dx * fx + dz * fz) / dl : 1.0f;
        if (cosang < std::cos(35.0f * 3.14159265f / 180.0f)) app.status_moving = true;
        if (app.status_moving) {
            const float k = 0.08f;
            p.pose.position.x += (target.x - p.pose.position.x) * k;
            p.pose.position.y += (target.y - p.pose.position.y) * k;
            p.pose.position.z += (target.z - p.pose.position.z) * k;
            const float ex = target.x - p.pose.position.x, ez = target.z - p.pose.position.z;
            if (ex * ex + ez * ez < 0.02f * 0.02f) app.status_moving = false;
        }
    }
    // Face the head: the quad's front (+Z) points from the panel to the head.
    const float yaw = std::atan2(hp.x - p.pose.position.x, hp.z - p.pose.position.z);
    p.pose.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
}

void Notice(App& app, const std::string& text, int seconds) {
    app.notice = text;
    app.notice_until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
}

std::string ControlName(const std::string& owner) {
    if (owner == "manual") return T("マニュアル(このヘッドセット)", "manual (this headset)");
    if (owner.empty() || owner == "any") return T("制限なし", "anyone");
    if (owner == "auto") return T("オート", "auto");
    return owner;
}


// What the panels say this frame.
void UpdatePanels(App& app, bool video, const XrSpaceLocation& head) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= app.next_address) {
        app.next_address = now + std::chrono::seconds(5);
        app.own_address = OwnAddress();
    }
    const bool linked = app.sender.Connected() && g_link.load() != kNone;
    const bool pc_fresh = app.pc.known && now - app.pc.at < std::chrono::seconds(3);
    const int link = g_link.load();
    std::string pc_name;
    {
        std::lock_guard<std::mutex> l(g_pc_mu);
        pc_name = g_pc_name;
    }
    const std::string where = link == kWifi ? Fmt("Wi-Fi %s", AddrText(g_pc_addr.load()).c_str())
                                            : std::string(T("USB ケーブル", "USB cable"));

    // Connection screen: whenever the PC's image is not showing.
    app.status_panel.visible = false;
    if (!video) {
        PlaceStatusPanel(app, head);
        std::vector<TextLine> l;
        l.push_back({kDev ? "VHMD Link Dev" : "VHMD Link", 54, 0xFFFFFFFF, true});
        if (!pc_fresh) {
            l.push_back({T("PC を探しています…", "Looking for the PC..."), 40, 0xFFFFC857, true, 26});
            l.push_back({T("1. PC で「VHMD Link」を起動します", "1. Start \"VHMD Link\" on the PC"), 30, 0xFFE6E6E6, false, 18});
            l.push_back({T("2. 同じ Wi-Fi につなぐか、USB ケーブルでつなぎます", "2. Use the same Wi-Fi, or connect a USB cable"),
                         30, 0xFFE6E6E6});
            l.push_back({T("   自動で見つけて接続します", "   It connects by itself"), 30, 0xFFB0B0B0});
        } else {
            l.push_back({Fmt(T("PC「%s」に接続しました", "Connected to \"%s\""), pc_name.empty() ? "PC" : pc_name.c_str()),
                         40, 0xFF6EE7A0, true, 26});
            l.push_back({std::string(T("接続: ", "Link: ")) + where, 30, 0xFFE6E6E6, false, 10});
            if (!app.pc.steamvr)
                l.push_back({T("SteamVR を待っています(PC で起動してください)", "Waiting for SteamVR (start it on the PC)"),
                             32, 0xFFFFC857, false, 18});
            else if (!app.pc.driver)
                l.push_back({T("SteamVR を準備しています…", "SteamVR is starting..."), 32, 0xFFFFC857, false, 18});
            else
                l.push_back({T("映像を待っています…", "Waiting for the image..."), 32, 0xFFFFC857, false, 18});
        }
        l.push_back({Fmt(T("このヘッドセット: %s   v%s", "This headset: %s   v%s"),
                         app.own_address.empty() ? T("ネットワークなし", "no network") : app.own_address.c_str(),
                         VHMD_LINK_VERSION),
                     24, 0xFF9AA4B2, false, 30});
        SetPanel(app, app.status_panel, 0xE0141821, 44, l);
    }

    // Who has control, announced when it changes; X + A held shows progress.
    // Announced when the headset loses or regains control (not at the start
    // when it has it).
    auto headset_moves = [](const std::string& c) { return c.empty() || c == "manual" || c == "any"; };
    if (pc_fresh && app.pc.control != app.shown_control) {
        const bool now_ok = headset_moves(app.pc.control);
        const bool was_ok = app.shown_control.empty() || headset_moves(app.shown_control);
        if (!now_ok)
            Notice(app, Fmt(T("%s が操作中 ― X + A 長押しで取り戻せます", "%s is in control - hold X + A to take it back"),
                            ControlName(app.pc.control).c_str()), 5);
        else if (!was_ok)
            Notice(app, T("このヘッドセットで操作しています", "This headset is in control again"), 4);
        app.shown_control = app.pc.control;
    }
    if (app.take_holding && !app.take_sent) Notice(app, T("X + A を押し続けると操作を取り戻します…", "Keep holding X + A to take control..."), 1);
    if (!linked && !pc_fresh && app.pc.known && video) Notice(app, T("PC との接続が切れました", "Lost the connection to the PC"), 2);
    if (now < app.notice_until && !app.notice.empty())
        SetPanel(app, app.notice_panel, 0xD0141821, 22, {{app.notice, 34, 0xFFFFFFFF, true}});
    else
        app.notice_panel.visible = false;

    // Dev edition: statistics under the line of sight while the image shows.
    if (kDev && video) {
        std::vector<TextLine> l;
        l.push_back({Fmt(T("映像 %.0f/%.0f fps  表示 %.0f Hz  %.1f Mbit/s  待ち %.1f ms",
                           "Video %.0f/%.0f fps  display %.0f Hz  %.1f Mbit/s  wait %.1f ms"),
                         app.st_shown, app.st_received, app.st_display, app.st_mbps, app.st_wait),
                     30, 0xFFFFFFFF, false});
        l.push_back({Fmt(T("%s  音 %.0f/s 途切れ %d  振動 %d  操作 %s", "%s  audio %.0f/s underruns %d  haptics %d  control %s"),
                         where.c_str(), app.st_audio, app.st_underruns, app.st_haptics,
                         pc_fresh ? ControlName(app.pc.control).c_str() : "?"),
                     26, 0xFFB8C4D6, false, 4});
        SetPanel(app, app.hud_panel, 0xB0101418, 18, l);
    } else {
        app.hud_panel.visible = false;
    }
}

void RenderEye(App& app, Swapchain& sc, uint32_t image, const XrView& view, const std::vector<Vertex>& verts,
               int eye, bool video) {
    glBindFramebuffer(GL_FRAMEBUFFER, sc.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.images[image].image, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sc.depth);
    glViewport(0, 0, sc.width, sc.height);
    if (video) {
        // This eye's half of VHMD's side-by-side image fills the view; the
        // layer carries the field of view it was rendered with.
        glDisable(GL_DEPTH_TEST);
        glUseProgram(app.video_prog);
        glUniform4f(app.uv_rect_loc, eye == 0 ? 0.0f : 0.5f, 0.0f, eye == 0 ? 0.5f : 1.0f, 1.0f);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, app.video.texture());
        glBindVertexArray(app.empty_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return;
    }
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.03f, 0.035f, 0.05f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const Mat4 mvp = Multiply(Projection(view.fov, 0.05f, 100.0f), InvertRigid(FromPose(view.pose)));
    glUseProgram(app.program);
    glUniformMatrix4fv(app.mvp_loc, 1, GL_FALSE, mvp.m);
    glBindVertexArray(app.vao);
    glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size()));
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void Frame(App& app) {
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (!Check(xrWaitFrame(app.session, nullptr, &fs), "xrWaitFrame")) return;
    xrBeginFrame(app.session, nullptr);

    XrActiveActionSet active{app.action_set, XR_NULL_PATH};
    XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
    sync.countActiveActionSets = 1;
    sync.activeActionSets = &active;
    xrSyncActions(app.session, &sync);

    // Poses for the PC.
    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION}, hands[2] = {{XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}};
    xrLocateSpace(app.head, app.base, fs.predictedDisplayTime, &head);
    for (int h = 0; h < 2; ++h) xrLocateSpace(app.grip_spaces[h], app.base, fs.predictedDisplayTime, &hands[h]);
    std::string msg = "[";
    AppendPose(msg, "hmd", head);
    AppendPose(msg, "left_hand", hands[kLeft]);
    AppendPose(msg, "right_hand", hands[kRight]);
    AppendInputs(app, msg, kLeft);
    AppendInputs(app, msg, kRight);

    // Vibration the PC asked for: {"type":"haptic","id":"left_hand","duration":s,"frequency":Hz,"amplitude":0..1}.
    for (const std::string& m : app.sender.TakeIncoming()) {
        if (m.find("\"type\":\"status\"") != std::string::npos) {
            // {"type":"status","pc":"NAME","steamvr":true,"driver":true,"control":"manual"}
            app.pc.known = true;
            app.pc.at = std::chrono::steady_clock::now();
            app.pc.steamvr = JsonBool(m, "steamvr", false);
            app.pc.driver = JsonBool(m, "driver", false);
            JsonString(m, "control", &app.pc.control);
            double refresh = 0;
            app.pc.refresh = JsonNumber(m, "refresh", &refresh) ? refresh : 0;
            std::string name;
            if (JsonString(m, "pc", &name)) {
                std::lock_guard<std::mutex> l(g_pc_mu);
                g_pc_name = name;
            }
            continue;
        }
        if (m.find("\"haptic\"") == std::string::npos) continue;
        const int hand = m.find("\"left_hand\"") != std::string::npos ? kLeft : kRight;
        double duration = 0, frequency = 0, amplitude = 0;
        JsonNumber(m, "duration", &duration);
        JsonNumber(m, "frequency", &frequency);
        JsonNumber(m, "amplitude", &amplitude);
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
        v.duration = duration > 0 ? static_cast<XrDuration>(duration * 1e9) : XR_MIN_HAPTIC_DURATION;
        v.frequency = frequency > 0 ? static_cast<float>(frequency) : XR_FREQUENCY_UNSPECIFIED;
        v.amplitude = static_cast<float>(std::clamp(amplitude, 0.0, 1.0));
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = app.haptic;
        hi.subactionPath = app.hand_paths[hand];
        xrApplyHapticFeedback(app.session, &hi, reinterpret_cast<XrHapticBaseHeader*>(&v));
        ++app.haptics;
    }

    // The refresh rate the PC asked for (VHMD then renders at it).
    if (app.pc.refresh > 0 && std::abs(app.pc.refresh - app.requested_refresh) > 0.5 && !app.refresh_rates.empty()) {
        float best = app.refresh_rates[0];
        for (float r : app.refresh_rates)
            if (std::abs(r - app.pc.refresh) < std::abs(best - app.pc.refresh)) best = r;
        PFN_xrRequestDisplayRefreshRateFB request_fn = nullptr;
        xrGetInstanceProcAddr(app.instance, "xrRequestDisplayRefreshRateFB",
                              reinterpret_cast<PFN_xrVoidFunction*>(&request_fn));
        const bool ok = request_fn && XR_SUCCEEDED(request_fn(app.session, best));
        LOGI("refresh rate %.0f Hz (PC asked %.0f): %s", best, app.pc.refresh, ok ? "ok" : "failed");
        app.requested_refresh = static_cast<float>(app.pc.refresh);
        app.next_info = {};   // tell the PC soon
    }

    // Holding X and A for 1.5 s gives control back to the person wearing the
    // headset (an automation may have it): {"op":"control","source":"manual"}.
    {
        const bool both = GetBool(app, app.btn_x, XR_NULL_PATH) && GetBool(app, app.btn_a, XR_NULL_PATH);
        const auto t = std::chrono::steady_clock::now();
        if (!both) {
            app.take_holding = app.take_sent = false;
        } else if (!app.take_holding) {
            app.take_holding = true;
            app.take_since = t;
        } else if (!app.take_sent && t - app.take_since >= std::chrono::milliseconds(1500)) {
            app.take_sent = true;
            msg += ",{\"type\":\"config\",\"op\":\"control\",\"source\":\"manual\"}";
            Notice(app, T("操作を取り戻しました(マニュアル)", "Control taken back (manual)"), 4);
            app.shown_control = "manual";
            LOGI("X+A held: taking control back (manual)");
        }
    }

    // Eye poses and fields of view relative to the head.
    std::vector<XrView> eye_local(app.swapchains.size(), {XR_TYPE_VIEW});
    {
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = app.head;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t n = 0;
        xrLocateViews(app.session, &li, &vs, static_cast<uint32_t>(eye_local.size()), &n, eye_local.data());
    }
    const auto now_info = std::chrono::steady_clock::now();
    app.video.SetPeriodMs(static_cast<float>(fs.predictedDisplayPeriod) / 1e6f);
    if (now_info >= app.next_timing) {
        app.next_timing = now_info + std::chrono::milliseconds(500);
        float slack = 0, weight = 0;
        int frames = 0;
        if (app.video.TakeTiming(&slack, &weight, &frames) && frames >= 10) {
            msg += Fmt(",{\"type\":\"link_timing\",\"slack_ms\":%.2f,\"period_ms\":%.3f,\"weight\":%.2f,\"n\":%d}", slack,
                       static_cast<float>(fs.predictedDisplayPeriod) / 1e6f, weight, frames);
        }
    }
    if (now_info >= app.next_info) {   // now and then: UDP may drop one
        app.next_info = now_info + std::chrono::seconds(2);
        AppendHeadsetInfo(app, msg, eye_local);
        if (!app.link_stats.empty()) {
            msg += ',';
            msg += app.link_stats;
        }
    }
    msg += "]\n";
    app.sender.Post(std::move(msg));

    std::vector<XrCompositionLayerProjectionView> proj_views;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::vector<XrCompositionLayerBaseHeader*> layers;
    std::vector<XrCompositionLayerQuad> quads;
    quads.reserve(3);
    if (fs.shouldRender) {
        XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
        vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        vli.displayTime = fs.predictedDisplayTime;
        vli.space = app.base;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        uint32_t vc = 0;
        std::vector<XrView> views(app.swapchains.size(), {XR_TYPE_VIEW});
        xrLocateViews(app.session, &vli, &vs, static_cast<uint32_t>(views.size()), &vc, views.data());

        // Scene: floor grid (green when the PC is connected, red when not) and
        // an axis triad on each controller.
        std::vector<Vertex> verts;
        const int link = app.sender.Connected() ? g_link.load() : kNone;
        const float gr = link == kNone ? 0.45f : 0.12f;
        const float gg = link == kNone ? 0.12f : 0.45f;
        const float gb = link == kUsb ? 0.5f : link == kWifi ? 0.2f : 0.12f;
        for (int i = -10; i <= 10; ++i) {
            const float t = i * 0.5f;
            Line(verts, t, 0, -5, t, 0, 5, gr, gg, gb);
            Line(verts, -5, 0, t, 5, 0, t, gr, gg, gb);
        }
        Line(verts, 0, 0.001f, 0, 1, 0.001f, 0, 1, 0.2f, 0.2f);    // +X
        Line(verts, 0, 0.001f, 0, 0, 0.001f, -1, 0.3f, 0.5f, 1);   // -Z (forward)
        for (int h = 0; h < 2; ++h) {
            if (!(hands[h].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) continue;
            if (kDev) {
                Triad(verts, hands[h].pose, 0.1f);
            } else {
                const XrVector3f a = hands[h].pose.position;
                const XrVector3f d = Rotate(hands[h].pose.orientation, {0, 0, -0.12f});
                Line(verts, a.x, a.y, a.z, a.x + d.x, a.y + d.y, a.z + d.z, 0.85f, 0.88f, 0.95f);
            }
        }
        glBindBuffer(GL_ARRAY_BUFFER, app.vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(verts.size() * sizeof(Vertex)), verts.data(),
                     GL_STREAM_DRAW);
        const bool video = app.video.Update(app.display);
        // With the pose the frame was rendered for, the layer is placed where it
        // belongs and the headset's compositor reprojects it to the current head
        // pose; without it the image follows the head.
        XrPosef frame_head{};
        const bool reproject = video && app.video.FramePose(&frame_head);
        app.reprojected += reproject ? 1 : 0;

        proj_views.resize(vc, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
        for (uint32_t i = 0; i < vc; ++i) {
            Swapchain& sc = app.swapchains[i];
            uint32_t image = 0;
            XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            xrAcquireSwapchainImage(sc.handle, &ai, &image);
            XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(sc.handle, &wi);
            RenderEye(app, sc, image, views[i], verts, static_cast<int>(i), video);
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(sc.handle, &ri);
            proj_views[i].pose = reproject && i < eye_local.size() ? Compose(frame_head, eye_local[i].pose) : views[i].pose;
            proj_views[i].fov = video && app.video.HasFov() ? app.video.Fov(static_cast<int>(i)) : views[i].fov;
            proj_views[i].subImage.swapchain = sc.handle;
            proj_views[i].subImage.imageRect = {{0, 0}, {sc.width, sc.height}};
        }
        layer.space = app.base;
        layer.viewCount = vc;
        layer.views = proj_views.data();
        layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));

        UpdatePanels(app, video, head);
        AddPanelLayer(app.status_panel, quads);
        AddPanelLayer(app.notice_panel, quads);
        AddPanelLayer(app.hud_panel, quads);
        for (auto& q : quads) layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&q));
    }

    static auto last_log = std::chrono::steady_clock::now();
    static int xr_frames = 0;
    ++xr_frames;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_log >= std::chrono::seconds(2)) {
        const Video::Stats v = app.video.TakeStats();
        const float s = std::chrono::duration<float>(now - last_log).count();
        const float rep = xr_frames ? 100.0f * app.reprojected / xr_frames : 0.0f;
        LOGI("frames %.0f/s, video received %.0f/s decoded %.0f/s shown %.0f/s (repeats %.0f/s, dropped %.0f/s, "
             "buffer %d, wait %.1f ms avg %.1f p95), at render pose %.0f%%, bunched: received %.0f/s decoded %.0f/s; "
             "audio %.0f packets/s, %d underruns, %d skipped; haptics %d",
             xr_frames / s, v.received / s, v.decoded / s, v.shown / s, v.repeats / s, v.dropped / s, v.buffer,
             v.wait_avg_ms, v.wait_p95_ms, rep, v.rx_close / s, v.dec_close / s, v.audio_packets / s,
             v.audio_underruns, v.audio_skipped, app.haptics);
        app.st_haptics = app.haptics;
        app.haptics = 0;
        // The same numbers for the PC: over Wi-Fi there is no logcat.
        char st[384];
        snprintf(st, sizeof(st),
                 "{\"type\":\"link_stats\",\"display\":%.1f,\"received\":%.1f,\"decoded\":%.1f,\"shown\":%.1f,"
                 "\"repeats\":%.1f,\"dropped\":%.1f,\"buffer\":%d,\"wait_ms\":%.1f,\"wait_p95_ms\":%.1f,"
                 "\"reprojected\":%.0f,\"audio_packets\":%.1f,\"audio_underruns\":%d,\"decode_ms\":%.2f,"
                 "\"decode_max_ms\":%.2f}",
                 xr_frames / s, v.received / s, v.decoded / s, v.shown / s, v.repeats / s, v.dropped / s, v.buffer,
                 v.wait_avg_ms, v.wait_p95_ms, rep, v.audio_packets / s, v.audio_underruns, v.decode_avg_ms,
                 v.decode_max_ms);
        app.link_stats = st;
        app.st_display = xr_frames / s;
        app.st_received = v.received / s;
        app.st_shown = v.shown / s;
        app.st_wait = v.wait_avg_ms;
        app.st_mbps = static_cast<float>(v.bytes) * 8.0f / 1e6f / s;
        app.st_audio = v.audio_packets / s;
        app.st_underruns = v.audio_underruns;
        app.reprojected = 0;
        xr_frames = 0;
        last_log = now;
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = static_cast<uint32_t>(layers.size());
    ei.layers = layers.data();
    xrEndFrame(app.session, &ei);
}

void PollEvents(App& app) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(app.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            app.state = e->state;
            LOGI("session state %d", static_cast<int>(app.state));
            if (app.state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                app.running = Check(xrBeginSession(app.session, &bi), "xrBeginSession");
            } else if (app.state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(app.session);
                app.running = false;
            } else if (app.state == XR_SESSION_STATE_EXITING || app.state == XR_SESSION_STATE_LOSS_PENDING) {
                app.exit = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            app.exit = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

}  // namespace

void android_main(android_app* android) {
    App app;
    app.android = android;
    android->userData = &app;
    android->onAppCmd = OnAppCmd;
    jobject wifi_lock = nullptr;
    {
        char lang[3] = {};
        AConfiguration_getLanguage(android->config, lang);
        g_ja = lang[0] == 'j' && lang[1] == 'a';
    }

    if (android->activity->externalDataPath) {
        const std::string path = std::string(android->activity->externalDataPath) + "/pc_address.txt";
        if (FILE* f = fopen(path.c_str(), "r")) {
            char text[64] = {};
            fgets(text, sizeof(text), f);
            fclose(f);
            text[strcspn(text, " \r\n")] = 0;
            in_addr a{};
            if (inet_pton(AF_INET, text, &a) == 1) {
                g_manual_addr = a.s_addr;
                LOGI("PC address from %s: %s", path.c_str(), text);
            }
        }
    }
    if (!InitLoader(app) || !CreateInstance(app) || !InitEgl(app) || !CreateSession(app) ||
        !CreateSwapchains(app) || !CreateActions(app)) {
        LOGE("startup failed");
        ANativeActivity_finish(android->activity);
    } else {
        InitGl(app);
        CreatePanels(app);
        wifi_lock = AcquireWifiLock(app);
        app.sender.Start();
        app.video.Start();
        LOGI("VHMD Link%s %s running; looking for the PC (Wi-Fi discovery on UDP %u, USB fallback)",
             kDev ? " Dev" : "", VHMD_LINK_VERSION, kDiscoveryPort);
    }

    while (!android->destroyRequested) {
        // Block on Android events only while the XR session is idle.
        for (;;) {
            int events = 0;
            android_poll_source* source = nullptr;
            const int timeout = (!app.running && !app.resumed && app.session == XR_NULL_HANDLE) ? -1 : 0;
            if (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) < 0) break;
            if (source) source->process(android, source);
            if (android->destroyRequested) break;
        }
        if (app.instance == XR_NULL_HANDLE || app.session == XR_NULL_HANDLE) continue;
        PollEvents(app);
        if (app.exit) {
            ANativeActivity_finish(android->activity);
            app.exit = false;
        }
        if (!app.running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        Frame(app);
    }

    ReleaseWifiLock(app, wifi_lock);
    app.sender.Stop();
    app.video.Stop();
    if (app.session) xrDestroySession(app.session);
    if (app.instance) xrDestroyInstance(app.instance);
    if (app.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(app.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (app.context != EGL_NO_CONTEXT) eglDestroyContext(app.display, app.context);
        if (app.surface != EGL_NO_SURFACE) eglDestroySurface(app.display, app.surface);
        eglTerminate(app.display);
    }
}
