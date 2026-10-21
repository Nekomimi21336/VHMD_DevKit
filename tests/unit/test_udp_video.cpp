// The headset app's UDP video receiver (headset/android/src/udp_video.h) against
// packets made by the PC side (link/proto, Rust): a 300,000-byte message in 375
// packets (3 Reed-Solomon blocks, 25 % parity) with 41 of them lost
// (link/proto/testdata/udp_vectors.bin).
#include "test_framework.h"

#include <fstream>
#include <iterator>
#include <vector>

#include "../../headset/android/src/udp_video.h"

#ifndef VHMD_SOURCE_DIR
#define VHMD_SOURCE_DIR "."
#endif

namespace {
uint32_t Le32(const std::vector<uint8_t>& v, size_t at) {
    return uint32_t(v[at]) | (uint32_t(v[at + 1]) << 8) | (uint32_t(v[at + 2]) << 16) | (uint32_t(v[at + 3]) << 24);
}
}  // namespace

TEST(udp_video_reassembles_rust_packets_with_loss) {
    std::ifstream f(VHMD_SOURCE_DIR "/link/proto/testdata/udp_vectors.bin", std::ios::binary);
    CHECK(f.good());
    std::vector<uint8_t> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(v.size() > 8);
    size_t at = 0;
    const uint32_t msg_len = Le32(v, at);
    at += 4;
    std::vector<uint8_t> expected(v.begin() + at, v.begin() + at + msg_len);
    at += msg_len;
    const uint32_t count = Le32(v, at);
    at += 4;
    CHECK(count == 375 - 41);
    vhmd_link::UdpReassembler r;
    std::vector<uint8_t> out, got;
    const auto now = vhmd_link::UdpReassembler::Clock::now();
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t n = Le32(v, at);
        at += 4;
        if (r.Push(v.data() + at, n, now, &out)) got = out;
        at += n;
    }
    CHECK(got == expected);
    CHECK(r.recovered == 1 && r.lost == 0 && r.delivered == 1);
}

TEST(udp_video_blocks_and_gf) {
    auto b = vhmd_link::UdpReassembler::Blocks(300, 25);
    CHECK(b.size() == 3);
    CHECK(std::get<1>(b[0]) == 128 && std::get<2>(b[0]) == 32 && std::get<0>(b[2]) == 256 && std::get<2>(b[2]) == 11);
    for (int a = 1; a < 256; ++a) CHECK(vhmd_link::gf256::Mul(uint8_t(a), vhmd_link::gf256::Inv(uint8_t(a))) == 1);
}
