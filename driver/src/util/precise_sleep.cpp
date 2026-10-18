#include "util/precise_sleep.h"

#include <thread>

#include "util/platform.h"

#if defined(VHMD_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace vhmd {

PreciseSleeper::PreciseSleeper() {
#if defined(VHMD_PLATFORM_WINDOWS)
    timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
#endif
}

PreciseSleeper::~PreciseSleeper() {
#if defined(VHMD_PLATFORM_WINDOWS)
    if (timer_) CloseHandle(static_cast<HANDLE>(timer_));
#endif
}

void PreciseSleeper::SleepUntil(std::chrono::steady_clock::time_point deadline) {
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now) return;
#if defined(VHMD_PLATFORM_WINDOWS)
    if (timer_) {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now).count();
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(ns / 100); // relative, 100 ns units
        if (SetWaitableTimerEx(static_cast<HANDLE>(timer_), &due, 0, nullptr, nullptr, nullptr, 0)) {
            WaitForSingleObject(static_cast<HANDLE>(timer_), INFINITE);
            return;
        }
    }
#endif
    std::this_thread::sleep_until(deadline);
}

} // namespace vhmd
