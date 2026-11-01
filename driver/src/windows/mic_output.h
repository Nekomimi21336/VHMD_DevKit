// Plays the headset's microphone into an audio output, by default VB-CABLE's
// "CABLE Input": Windows programs then take "CABLE Output" as their microphone.
// A driver cannot add a recording device by itself (that takes a signed audio
// driver), so the output has to exist already. The monitor plays it into the
// default output too, which also goes back to the headset: the person hears
// what went through the PC.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "protocol/messages.h"
#include "state/mic_buffer.h"

namespace vhmd {

// One output and its thread.
class MicPlayer {
public:
    ~MicPlayer() { Stop(); }

    // The output whose name contains `name`; "" = off, kDefaultOutput = the default output.
    void SetTarget(const std::string& name);
    // Never plays into an output with this name: the sound would come back into itself.
    void SetAvoid(const std::string& name);
    void Push(const MicMessage& m);
    std::string Target() const;
    std::string State() const;
    MicBuffer::Stats Stats() const { return buffer_.GetStats(); }
    void Stop();

    static constexpr const char* kDefaultOutput = "*";

private:
    void Loop();
    void SetState(const char* state);

    static constexpr size_t kRate = 48000;
    MicBuffer buffer_{kRate * 40 / 1000, kRate * 120 / 1000};   // starts at 40 ms, at most 120 ms
    mutable std::mutex mu_;
    std::string target_, avoid_, state_ = "off";
    std::thread thread_;
    std::atomic<bool> running_{false};
};

class MicOutput {
public:
    void SetDevice(const std::string& name);
    void SetMonitor(bool on) { monitor_.SetTarget(on ? MicPlayer::kDefaultOutput : ""); }
    void Push(const MicMessage& m);
    std::string StatusJson() const;
    void Stop();

private:
    MicPlayer output_, monitor_;
};

} // namespace vhmd
