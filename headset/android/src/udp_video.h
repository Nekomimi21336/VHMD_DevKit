// VHMD Link: video over UDP with forward error correction, receiving side.
// The packet format and the Reed-Solomon parity are described in
// link/proto/src/udp.rs (the sending side); this is the same reassembly in C++
// for the headset app. Standard library only, so the PC's unit tests check it
// against packets the Rust side made (tests/unit/test_udp_video.cpp).
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

namespace vhmd_link {

namespace gf256 {
struct Tables {
    uint8_t exp[512];
    uint8_t log[256];
    Tables() {
        unsigned x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[i] = uint8_t(x);
            log[x] = uint8_t(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x11D;
        }
        for (int i = 255; i < 512; ++i) exp[i] = exp[i - 255];
        log[0] = 0;
    }
};
inline const Tables& T() {
    static const Tables t;
    return t;
}
inline uint8_t Mul(uint8_t a, uint8_t b) { return (a && b) ? T().exp[T().log[a] + T().log[b]] : 0; }
inline uint8_t Inv(uint8_t a) { return T().exp[255 - T().log[a]]; }
inline void MulAdd(uint8_t* dst, const uint8_t* src, size_t n, uint8_t c) {
    if (!c) return;
    uint8_t t[256];
    for (int v = 0; v < 256; ++v) t[v] = Mul(c, uint8_t(v));
    for (size_t i = 0; i < n; ++i) dst[i] ^= t[src[i]];
}
inline uint8_t Cauchy(size_t i, size_t j, size_t m) { return Inv(uint8_t(i) ^ uint8_t(m + j)); }

// Inverts an n x n matrix (row-major); false if singular.
inline bool Invert(std::vector<uint8_t>& a, size_t n, std::vector<uint8_t>* out) {
    std::vector<uint8_t>& b = *out;
    b.assign(n * n, 0);
    for (size_t i = 0; i < n; ++i) b[i * n + i] = 1;
    for (size_t c = 0; c < n; ++c) {
        size_t p = c;
        while (p < n && !a[p * n + c]) ++p;
        if (p == n) return false;
        if (p != c)
            for (size_t x = 0; x < n; ++x) {
                std::swap(a[c * n + x], a[p * n + x]);
                std::swap(b[c * n + x], b[p * n + x]);
            }
        const uint8_t f = Inv(a[c * n + c]);
        for (size_t x = 0; x < n; ++x) {
            a[c * n + x] = Mul(a[c * n + x], f);
            b[c * n + x] = Mul(b[c * n + x], f);
        }
        for (size_t r = 0; r < n; ++r) {
            const uint8_t g = a[r * n + c];
            if (r == c || !g) continue;
            for (size_t x = 0; x < n; ++x) {
                a[r * n + x] ^= Mul(g, a[c * n + x]);
                b[r * n + x] ^= Mul(g, b[c * n + x]);
            }
        }
    }
    return true;
}
}  // namespace gf256

class UdpReassembler {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr size_t kHeader = 20, kBlock = 128;

    std::chrono::milliseconds wait{100};
    uint64_t packets = 0, delivered = 0, recovered = 0, lost = 0;

    // (first data index, data count, parity count) of each block.
    static std::vector<std::tuple<size_t, size_t, size_t>> Blocks(size_t data_count, size_t fec) {
        std::vector<std::tuple<size_t, size_t, size_t>> out;
        for (size_t at = 0; at < data_count;) {
            const size_t k = std::min(kBlock, data_count - at);
            const size_t m = fec == 0 ? 0 : std::clamp<size_t>((k * fec + 99) / 100, 1, 255 - k);
            out.emplace_back(at, k, m);
            at += k;
        }
        return out;
    }

    // One packet; true with the completed message in *out.
    bool Push(const uint8_t* p, size_t n, Clock::time_point now, std::vector<uint8_t>* out) {
        if (n < kHeader || std::memcmp(p, "VHU2", 4) != 0) return false;   // byte 12: fec, 13: kind
        ++packets;
        const uint32_t seq = U32(p + 4);
        const size_t index = U16(p + 8), data_count = U16(p + 10), fec = p[12], frag = U16(p + 14);
        const uint8_t kind = p[13];
        const size_t size = U32(p + 16);
        if (has_last_ && !Newer(seq, last_out_)) return false;   // delivered or given up already
        if (data_count == 0 || frag == 0 || size > data_count * frag) return false;
        Partial& m = partial_[seq];
        if (m.frags.empty()) {
            m.data_count = data_count;
            m.fec = fec;
            m.kind = kind;
            m.frag = frag;
            m.size = size;
            size_t parity = 0;
            for (auto& b : Blocks(data_count, fec)) parity += std::get<2>(b);
            m.frags.resize(data_count + parity);
            m.have.assign(data_count + parity, false);
            m.created = now;
        }
        if (index >= m.frags.size() || m.data_count != data_count) return false;
        if (!m.have[index]) {
            m.frags[index].assign(p + kHeader, p + n);
            m.have[index] = true;
        }
        bool rec = false;
        if (!Complete(m, &rec)) return false;
        out->clear();
        out->reserve(m.size);
        for (size_t i = 0; i < m.data_count; ++i) out->insert(out->end(), m.frags[i].begin(), m.frags[i].end());
        out->resize(m.size);
        partial_.erase(seq);
        // Older unfinished messages will not be shown any more.
        for (auto it = partial_.begin(); it != partial_.end();) {
            if (Newer(seq, it->first)) {
                NoteLost(it->second.kind);
                it = partial_.erase(it);
            } else {
                ++it;
            }
        }
        last_out_ = seq;
        has_last_ = true;
        ++delivered;
        recovered += rec ? 1 : 0;
        return true;
    }

    // Gives up on messages waiting longer than `wait`.
    void Expire(Clock::time_point now) {
        for (auto it = partial_.begin(); it != partial_.end();) {
            if (now - it->second.created > wait) {
                NoteLost(it->second.kind);
                it = partial_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Messages lost since the last call, and how many of them were video frames
    // (a lost frame needs a keyframe; lost sound does not).
    uint64_t TakeLost(uint64_t* frames = nullptr) {
        const uint64_t n = lost_new_;
        if (frames) *frames = lost_frames_new_;
        lost_new_ = lost_frames_new_ = 0;
        return n;
    }

private:
    struct Partial {
        size_t data_count = 0, fec = 0, frag = 0, size = 0;
        uint8_t kind = 0;
        std::vector<std::vector<uint8_t>> frags;
        std::vector<bool> have;
        Clock::time_point created{};
    };

    static uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
    static uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
    static bool Newer(uint32_t a, uint32_t b) { return int32_t(a - b) > 0; }

    void NoteLost(uint8_t kind) {
        ++lost;
        ++lost_new_;
        if (kind == 2) ++lost_frames_new_;
    }

    static bool Complete(Partial& m, bool* recovered) {
        size_t parity_at = m.data_count;
        for (auto& [start, k, mm] : Blocks(m.data_count, m.fec)) {
            std::vector<size_t> missing, have_parity;
            for (size_t j = 0; j < k; ++j)
                if (!m.have[start + j]) missing.push_back(j);
            if (!missing.empty()) {
                for (size_t i = 0; i < mm; ++i)
                    if (m.have[parity_at + i]) have_parity.push_back(i);
                if (have_parity.size() < missing.size()) return false;
                // Rows: the data fragments we have (identity), parity rows for the rest.
                std::vector<uint8_t> rows(k * k, 0);
                std::vector<std::vector<uint8_t>> srcs(k);
                size_t pi = 0;
                for (size_t j = 0; j < k; ++j) {
                    if (m.have[start + j]) {
                        rows[j * k + j] = 1;
                        srcs[j] = m.frags[start + j];
                    } else {
                        const size_t i = have_parity[pi++];
                        for (size_t c = 0; c < k; ++c) rows[j * k + c] = gf256::Cauchy(i, c, mm);
                        srcs[j] = m.frags[parity_at + i];
                    }
                    srcs[j].resize(m.frag, 0);
                }
                std::vector<uint8_t> inv;
                if (!gf256::Invert(rows, k, &inv)) return false;
                for (size_t j : missing) {
                    std::vector<uint8_t> v(m.frag, 0);
                    for (size_t c = 0; c < k; ++c) gf256::MulAdd(v.data(), srcs[c].data(), m.frag, inv[j * k + c]);
                    const size_t idx = start + j;
                    v.resize(idx + 1 == m.data_count ? m.size - idx * m.frag : m.frag);
                    m.frags[idx] = std::move(v);
                    m.have[idx] = true;
                }
                *recovered = true;
            }
            parity_at += mm;
        }
        return true;
    }

    std::map<uint32_t, Partial> partial_;
    uint32_t last_out_ = 0;
    bool has_last_ = false;
    uint64_t lost_new_ = 0, lost_frames_new_ = 0;
};

}  // namespace vhmd_link
