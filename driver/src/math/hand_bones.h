#pragma once
// A tracked hand as SteamVR's 31 hand bones, from the 26 joints of
// XR_EXT_hand_tracking. Used by the headset app; it has no other dependency.
//
// The joints and the bones share their numbers from the wrist (1) to the little
// finger's tip (25). OpenXR's joints all look the same way: -Z to the fingertip,
// +Y out of the back of the hand or the nail. SteamVR's bones differ by hand and
// the thumb is turned a quarter: found from the bones of OpenVR's hand model.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace vhmd {

struct HandQuat {
    float w = 1, x = 0, y = 0, z = 0;
};
struct HandVec {
    float x = 0, y = 0, z = 0;
};
struct HandPose {
    HandQuat rot;
    HandVec pos;
};

constexpr size_t kHandJoints = 26, kHandBones = 31;

// Where SteamVR's hand model puts the device, seen from the tracked wrist. Left, right.
inline constexpr HandPose kDeviceInWrist[2] = {
    {{0.7063f, 0.3072f, -0.2294f, -0.5951f}, {0.0944f, -0.0106f, -0.1435f}},
    {{0.7063f, 0.3072f, 0.2294f, 0.5951f}, {-0.0944f, -0.0106f, -0.1435f}},
};

namespace hand_detail {

inline HandQuat Mul(const HandQuat& a, const HandQuat& b) {
    return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
}
inline HandQuat Inverse(const HandQuat& q) { return {q.w, -q.x, -q.y, -q.z}; }
inline HandVec Add(const HandVec& a, const HandVec& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline HandVec Sub(const HandVec& a, const HandVec& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline HandVec Rotate(const HandQuat& q, const HandVec& v) {
    const HandQuat r = Mul(Mul(q, {0, v.x, v.y, v.z}), Inverse(q));
    return {r.x, r.y, r.z};
}

// The bone's axes as seen in the joint's: quaternions of these turns.
constexpr float kS = 0.70710678f;
inline constexpr HandQuat kFingerAxes[2] = {{0, kS, 0, -kS}, {kS, 0, -kS, 0}};   // left, right
inline constexpr HandQuat kThumbAxes[2] = {{0.5f, 0.5f, 0.5f, -0.5f}, {0.5f, -0.5f, -0.5f, -0.5f}};

inline int ParentBone(size_t bone) {
    switch (bone) {
    case 0: case 26: case 27: case 28: case 29: case 30: return -1;   // from the root
    case 1: return 0;
    case 2: case 6: case 11: case 16: case 21: return 1;              // from the wrist
    default: return static_cast<int>(bone) - 1;
    }
}

}  // namespace hand_detail

// `joints`: pose of each joint in the wrist's space (joint 1 is that of the wrist).
// Returns each bone in its parent's space, for hand 0 = left, 1 = right.
inline std::array<HandPose, kHandBones> HandBones(const std::array<HandPose, kHandJoints>& joints, int hand) {
    using namespace hand_detail;
    const HandQuat hand_in_device = Inverse(kDeviceInWrist[hand].rot);
    std::array<HandPose, kHandBones> device{};   // in the device's space
    std::array<HandPose, kHandBones> bones{};

    // The wrist bone stays where the hand model has it, so the device pose holds.
    const float flip = hand == 0 ? 1.0f : -1.0f;
    device[1].rot = {-0.055147f, -0.078608f, -0.920279f * flip, 0.379296f * flip};
    device[1].pos = {-0.034038f * flip, 0.036503f, 0.164722f};
    bones[1] = device[1];

    for (size_t i = 2; i < kHandJoints; ++i) {
        const HandQuat axes = i <= 5 ? kThumbAxes[hand] : kFingerAxes[hand];
        device[i].rot = Mul(Mul(hand_in_device, joints[i].rot), axes);
        device[i].pos = Add(Rotate(hand_in_device, joints[i].pos), device[1].pos);
        const HandPose& parent = device[static_cast<size_t>(ParentBone(i))];
        const HandQuat inv = Inverse(parent.rot);
        bones[i].rot = Mul(inv, device[i].rot);
        bones[i].pos = Rotate(inv, Sub(device[i].pos, parent.pos));
    }
    return bones;
}

// Thumb tip to index tip: 1 touching .. 0 apart, for the trigger.
inline float HandPinch(const std::array<HandPose, kHandJoints>& joints) {
    using namespace hand_detail;
    constexpr size_t kThumbTip = 5, kIndexTip = 10;
    const HandVec d = Sub(joints[kThumbTip].pos, joints[kIndexTip].pos);
    const float gap = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    return 1.0f - std::clamp((gap - 0.015f) / 0.025f, 0.0f, 1.0f);
}

// One bone as 7 int16: rotation w x y z in 1/32767, position in 0.1 mm.
inline void PackBone(const HandPose& b, int16_t* out) {
    const auto rot = [](float v) { return static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f)); };
    const auto pos = [](float v) { return static_cast<int16_t>(std::lround(std::clamp(v, -3.2f, 3.2f) * 10000.0f)); };
    const int16_t values[7] = {rot(b.rot.w), rot(b.rot.x), rot(b.rot.y), rot(b.rot.z), pos(b.pos.x), pos(b.pos.y), pos(b.pos.z)};
    std::copy(values, values + 7, out);
}

}  // namespace vhmd
