// Plays the headset's microphone into an audio output, by default VB-CABLE's
// "CABLE Input": Windows programs then take "CABLE Output" as their microphone.
// A driver cannot add a recording device by itself (that takes a signed audio
// driver), so the output has to exist already.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "protocol/messages.h"
#include "state/mic_buffer.h"

namespace vhmd {

class MicOutput {
public:
    ~MicOutput() { Stop(); }

    // The output whose name contains `name`; "" = off.
    void SetDevice(const std::string& name);
    void Push(const MicMessage& m) { buffer_.Push(m.pcm.data(), m.pcm.size(), m.seq); }
    std::string StatusJson() const;
    void Stop();

private:
    void Loop();
    std::string Device() const;
    void SetState(const char* state);

    static constexpr size_t kRate = 48000;
    MicBuffer buffer_{kRate * 40 / 1000, kRate * 120 / 1000};   // starts at 40 ms, at most 120 ms
    mutable std::mutex mu_;
    std::string device_, state_ = "off";
    std::thread thread_;
    std::atomic<bool> running_{false};
};

} // namespace vhmd
