// Records what drives the devices (config op "record"): every JSON datagram that
// moved a device, one line each with its arrival time ({"t":seconds,"d":datagram}),
// for vhmd_client.replay to send again at the same pace. Files go into one folder
// (the user's VHMD data folder), never to a path a sender names. No OpenVR
// dependency.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace vhmd {

class Recorder {
public:
    using Clock = std::chrono::steady_clock;

    // `dir` empty: DefaultDir().
    explicit Recorder(std::filesystem::path dir = {});

    // Starts a new file `name` (.jsonl added; empty = a name from the time).
    // Returns an error, or "" on success.
    std::string Start(const std::string& name, Clock::time_point now);
    void Stop();
    bool Recording() const;
    // One datagram that drove a device (JSON text). Microphone sound is left out.
    void Add(std::string_view datagram, Clock::time_point now);
    // {"recording", "path", "seconds", "lines", "bytes"} (and the last file after Stop).
    nlohmann::json Status(Clock::time_point now) const;

    // %LOCALAPPDATA%\VHMD\recordings, or $XDG_STATE_HOME/vhmd/recordings
    // (~/.local/state/vhmd/recordings), like the Python client's data folder.
    static std::filesystem::path DefaultDir();
    // Letters, digits, '-', '_' and '.' (not leading), at most 64; else empty.
    static std::string SafeName(const std::string& name);

    static constexpr uint64_t kMaxBytes = 512ull * 1024 * 1024;   // stops by itself beyond this

private:
    mutable std::mutex mu_;
    std::filesystem::path dir_, path_;
    std::ofstream out_;
    Clock::time_point started_{};
    double seconds_ = 0;
    uint64_t lines_ = 0, bytes_ = 0;
    bool recording_ = false;
};

} // namespace vhmd
