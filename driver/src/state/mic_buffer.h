// The headset microphone's sound between the network and the audio device. It
// plays once `start` samples wait, so a late packet does not stop it, and drops
// back to `start` above `limit`, so it never falls behind. No OpenVR or Windows
// dependency.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

namespace vhmd {

class MicBuffer {
public:
    struct Stats {
        uint64_t packets = 0;
        uint64_t lost = 0;        // packets that never came
        uint64_t underruns = 0;   // ran empty while playing
        uint64_t skipped = 0;     // samples dropped to catch up
    };

    MicBuffer(size_t start, size_t limit) : start_(start), limit_(limit) {}

    void Push(const int16_t* data, size_t count, uint32_t seq) {
        std::lock_guard<std::mutex> lock(mu_);
        if (stats_.packets > 0 && seq > next_seq_) stats_.lost += seq - next_seq_;
        next_seq_ = seq + 1;
        ++stats_.packets;
        samples_.insert(samples_.end(), data, data + count);
        if (samples_.size() > limit_) {
            const size_t drop = samples_.size() - start_;
            samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(drop));
            stats_.skipped += drop;
        }
    }

    // Fills `count` samples, with silence where there is no sound. Returns how many are sound.
    size_t Pull(int16_t* out, size_t count) {
        std::lock_guard<std::mutex> lock(mu_);
        if (!playing_ && samples_.size() >= start_ && !samples_.empty()) playing_ = true;
        const size_t sound = playing_ ? std::min(count, samples_.size()) : 0;
        std::copy_n(samples_.begin(), sound, out);
        std::fill(out + sound, out + count, int16_t{0});
        samples_.erase(samples_.begin(), samples_.begin() + static_cast<std::ptrdiff_t>(sound));
        if (playing_ && sound < count) {
            playing_ = false;
            ++stats_.underruns;
        }
        return sound;
    }

    Stats GetStats() const {
        std::lock_guard<std::mutex> lock(mu_);
        return stats_;
    }

private:
    const size_t start_, limit_;
    mutable std::mutex mu_;
    std::deque<int16_t> samples_;
    bool playing_ = false;
    uint32_t next_seq_ = 0;
    Stats stats_;
};

} // namespace vhmd
