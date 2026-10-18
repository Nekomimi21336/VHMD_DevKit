// Logging front-end. The driver installs a sink that forwards to
// vr::VRDriverLog() (ends up in vrserver.txt); tests use the default stderr sink.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace vhmd {

enum class LogLevel { Debug, Info, Warning, Error };

using LogSink = std::function<void(LogLevel, const std::string&)>;

void SetLogSink(LogSink sink);
void SetVerbose(bool verbose);
void Log(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

// Token bucket limiter for noisy log sites (e.g. invalid packets).
class RateLimiter {
public:
    explicit RateLimiter(double per_second = 5.0, double burst = 5.0) : rate_(per_second), burst_(burst), tokens_(burst) {}

    // Returns true if the event may be logged. `suppressed` receives the number
    // of events dropped since the last allowed one.
    bool Allow(uint64_t& suppressed) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_).count();
        last_ = now;
        tokens_ = std::min(burst_, tokens_ + dt * rate_);
        if (tokens_ >= 1.0) {
            tokens_ -= 1.0;
            suppressed = dropped_;
            dropped_ = 0;
            return true;
        }
        ++dropped_;
        return false;
    }

    void SetRate(double per_second) {
        std::lock_guard<std::mutex> lock(mu_);
        rate_ = per_second;
        burst_ = std::max(1.0, per_second);
    }

private:
    std::mutex mu_;
    double rate_;
    double burst_;
    double tokens_;
    uint64_t dropped_ = 0;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

} // namespace vhmd
