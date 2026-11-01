#pragma once
// A tracked hand as finger curls and a pinch, from the 26 joints of
// XR_EXT_hand_tracking (palm, wrist, then each finger from its base to the tip).
// Used by the headset app; it has no other dependency.

#include <algorithm>
#include <array>
#include <cmath>

namespace vhmd {

struct JointPos {
    float x = 0, y = 0, z = 0;
};
using HandJoints = std::array<JointPos, 26>;

namespace hand_detail {

inline JointPos Sub(const JointPos& a, const JointPos& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline float Dot(const JointPos& a, const JointPos& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Length(const JointPos& a) { return std::sqrt(Dot(a, a)); }

// How far the finger turns at joint b, in degrees (0 = straight).
inline float Bend(const JointPos& a, const JointPos& b, const JointPos& c) {
    const JointPos u = Sub(b, a), v = Sub(c, b);
    const float n = Length(u) * Length(v);
    if (n <= 0.0f) return 0.0f;
    return std::acos(std::clamp(Dot(u, v) / n, -1.0f, 1.0f)) * 57.29578f;
}

inline float Ramp(float value, float low, float high) { return std::clamp((value - low) / (high - low), 0.0f, 1.0f); }

}  // namespace hand_detail

// Thumb to pinky, 0 open .. 1 closed: the turns along each finger added up.
inline std::array<float, 5> JointCurls(const HandJoints& j) {
    using namespace hand_detail;
    constexpr int kThumb = 2, kFirstFinger = 6, kPerFinger = 5;
    std::array<float, 5> out{};
    out[0] = Ramp(Bend(j[kThumb], j[kThumb + 1], j[kThumb + 2]) + Bend(j[kThumb + 1], j[kThumb + 2], j[kThumb + 3]), 20.0f, 110.0f);
    for (int f = 0; f < 4; ++f) {
        const int b = kFirstFinger + f * kPerFinger;
        const float turn = Bend(j[b], j[b + 1], j[b + 2]) + Bend(j[b + 1], j[b + 2], j[b + 3]) + Bend(j[b + 2], j[b + 3], j[b + 4]);
        out[f + 1] = Ramp(turn, 20.0f, 190.0f);
    }
    return out;
}

// Thumb tip to index tip: 1 touching .. 0 apart, for the trigger.
inline float JointPinch(const HandJoints& j) {
    using namespace hand_detail;
    constexpr int kThumbTip = 5, kIndexTip = 10;
    return 1.0f - Ramp(Length(Sub(j[kThumbTip], j[kIndexTip])), 0.015f, 0.04f);
}

}  // namespace vhmd
