// Sleep until a deadline with sub-millisecond accuracy without changing the
// global timer resolution (no timeBeginPeriod). Windows uses a
// high-resolution waitable timer; other platforms use sleep_until.
#pragma once

#include <chrono>

namespace vhmd {

class PreciseSleeper {
public:
    PreciseSleeper();
    ~PreciseSleeper();
    PreciseSleeper(const PreciseSleeper&) = delete;
    PreciseSleeper& operator=(const PreciseSleeper&) = delete;

    void SleepUntil(std::chrono::steady_clock::time_point deadline);

private:
    void* timer_ = nullptr;
};

} // namespace vhmd
