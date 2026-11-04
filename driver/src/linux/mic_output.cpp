#include "mic_output.h"

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/log.h"

// libpulse-simple, loaded when first needed: PulseAudio, and PipeWire through
// pipewire-pulse. Without it the microphone reports "no audio" and the driver
// runs as before. Declarations from <pulse/simple.h> (stable C API).
namespace {

struct pa_simple;
struct pa_sample_spec {
    int format;
    uint32_t rate;
    uint8_t channels;
};
struct pa_buffer_attr {
    uint32_t maxlength, tlength, prebuf, minreq, fragsize;
};
constexpr int kSampleS16le = 3;      // PA_SAMPLE_S16LE
constexpr int kStreamPlayback = 1;   // PA_STREAM_PLAYBACK
constexpr int kErrNoEntity = 5;      // PA_ERR_NOENTITY: no such sink

struct Pulse {
    pa_simple* (*open)(const char*, const char*, int, const char*, const char*, const pa_sample_spec*, const void*,
                       const pa_buffer_attr*, int*) = nullptr;
    int (*write)(pa_simple*, const void*, size_t, int*) = nullptr;
    void (*free)(pa_simple*) = nullptr;
    const char* (*error)(int) = nullptr;

    static const Pulse* Get() {
        static const Pulse p = [] {
            Pulse out;
            void* lib = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
            if (!lib) return out;
            out.open = reinterpret_cast<decltype(out.open)>(dlsym(lib, "pa_simple_new"));
            out.write = reinterpret_cast<decltype(out.write)>(dlsym(lib, "pa_simple_write"));
            out.free = reinterpret_cast<decltype(out.free)>(dlsym(lib, "pa_simple_free"));
            out.error = reinterpret_cast<decltype(out.error)>(dlsym(lib, "pa_strerror"));   // from libpulse, its dependency
            return out;
        }();
        return p.open && p.write && p.free ? &p : nullptr;
    }
};

} // namespace

namespace vhmd {

void MicPlayer::SetTarget(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (name == target_) return;
        target_ = name;
    }
    if (!name.empty() && !running_.exchange(true)) thread_ = std::thread(&MicPlayer::Loop, this);
}

void MicPlayer::SetAvoid(const std::string& name) {
    std::lock_guard<std::mutex> lock(mu_);
    avoid_ = name;
}

void MicPlayer::Push(const MicMessage& m) {
    if (!Target().empty()) buffer_.Push(m.pcm.data(), m.pcm.size(), m.seq);
}

std::string MicPlayer::Target() const {
    std::lock_guard<std::mutex> lock(mu_);
    return target_;
}

std::string MicPlayer::State() const {
    std::lock_guard<std::mutex> lock(mu_);
    return state_;
}

void MicPlayer::SetState(const char* state) {
    std::lock_guard<std::mutex> lock(mu_);
    state_ = state;
}

void MicPlayer::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void MicPlayer::Loop() {
    const Pulse* pa = Pulse::Get();
    const pa_sample_spec spec{kSampleS16le, static_cast<uint32_t>(kRate), 1};
    // About 40 ms queued in the server, written 10 ms at a time (as on Windows).
    const uint32_t bytes_10ms = static_cast<uint32_t>(kRate / 100 * sizeof(int16_t));
    const pa_buffer_attr attr{static_cast<uint32_t>(-1), bytes_10ms * 4, bytes_10ms, bytes_10ms, static_cast<uint32_t>(-1)};
    std::vector<int16_t> chunk(kRate / 100);
    while (running_) {
        const std::string name = Target();
        if (name.empty()) {
            SetState("off");
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        if (!pa) {
            SetState("no audio");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        std::string avoid;
        {
            std::lock_guard<std::mutex> lock(mu_);
            avoid = avoid_;
        }
        if (!avoid.empty() && name == avoid) {
            SetState("same device");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        int err = 0;
        const bool by_default = name == kDefaultOutput;
        pa_simple* s = pa->open(nullptr, "VHMD", kStreamPlayback, by_default ? nullptr : name.c_str(),
                                "headset microphone", &spec, nullptr, &attr, &err);
        if (!s) {
            Log(LogLevel::Warning, "mic: cannot open \"%s\": %s", name.c_str(), pa->error ? pa->error(err) : "error");
            SetState(err == kErrNoEntity ? "no device" : "device error");
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        SetState("playing");
        Log(LogLevel::Info, "mic: playing into \"%s\"", name.c_str());
        while (running_ && Target() == name) {
            buffer_.Pull(chunk.data(), chunk.size());
            if (pa->write(s, chunk.data(), chunk.size() * sizeof(int16_t), &err) < 0) break;   // blocks: paced by the server
        }
        pa->free(s);
    }
    SetState("off");
}

void MicOutput::SetDevice(const std::string& name) {
    output_.SetTarget(name);
    monitor_.SetAvoid(name);
}

void MicOutput::Push(const MicMessage& m) {
    output_.Push(m);
    monitor_.Push(m);
}

std::string MicOutput::StatusJson() const {
    const MicBuffer::Stats s = output_.Stats();
    return nlohmann::json{{"device", output_.Target()}, {"state", output_.State()}, {"monitor", monitor_.State()},
                          {"packets", s.packets},       {"lost", s.lost},          {"underruns", s.underruns},
                          {"skipped", s.skipped}}
        .dump();
}

void MicOutput::Stop() {
    output_.Stop();
    monitor_.Stop();
}

} // namespace vhmd
