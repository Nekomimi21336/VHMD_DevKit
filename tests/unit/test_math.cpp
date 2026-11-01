#include "test_framework.h"

#include "math/finger_curls.h"
#include "math/hand_bones.h"
#include "math/pose_math.h"
#include "protocol/osc.h"

using namespace vhmd;

namespace {
constexpr double kPi = 3.14159265358979323846;
}

TEST(hand_bones_keep_the_joints_on_steamvrs_axes) {
    using hand_detail::Add;
    using hand_detail::Inverse;
    using hand_detail::Mul;
    using hand_detail::Rotate;
    for (int hand = 0; hand < 2; ++hand) {
        // Straight fingers along -Z from the wrist, every joint turned like the wrist.
        std::array<HandPose, kHandJoints> j{};
        auto finger = [&](size_t first, size_t count, float x) {
            for (size_t i = 0; i < count; ++i) j[first + i].pos = {x, 0.01f, -0.03f * float(i + 1)};
        };
        finger(2, 4, 0.05f);
        for (size_t f = 0; f < 4; ++f) finger(6 + f * 5, 5, 0.02f * float(f));
        const auto bones = HandBones(j, hand);

        const float along = hand == 0 ? 0.03f : -0.03f;   // the next joint is on the bone's X
        for (size_t bone : {size_t(3), size_t(4), size_t(7), size_t(8), size_t(13), size_t(25)}) {
            CHECK_NEAR(bones[bone].pos.x, along, 1e-4);
            CHECK_NEAR(bones[bone].pos.y, 0.0, 1e-4);
            CHECK_NEAR(bones[bone].pos.z, 0.0, 1e-4);
            CHECK_NEAR(bones[bone].rot.w, 1.0, 1e-4);     // straight: no turn between bones
        }

        // Back along the chain, the index fingertip lands where the joint says.
        HandPose at = bones[1];
        for (size_t bone : {size_t(6), size_t(7), size_t(8), size_t(9), size_t(10)}) {
            at.pos = Add(at.pos, Rotate(at.rot, bones[bone].pos));
            at.rot = Mul(at.rot, bones[bone].rot);
        }
        const HandVec want = Add(Rotate(Inverse(kDeviceInWrist[hand].rot), j[10].pos), bones[1].pos);
        CHECK_NEAR(at.pos.x, want.x, 1e-3);
        CHECK_NEAR(at.pos.y, want.y, 1e-3);
        CHECK_NEAR(at.pos.z, want.z, 1e-3);
    }

    std::array<HandPose, kHandJoints> j{};
    j[10].pos = {0.1f, 0, 0};
    CHECK(HandPinch(j) == 0.0f);          // tips apart
    j[5].pos = {0.095f, 0, 0};
    CHECK(HandPinch(j) == 1.0f);          // touching

    int16_t packed[7];
    PackBone({{1, 0, 0, 0}, {0.1f, -0.2f, 0}}, packed);
    CHECK(packed[0] == 32767 && packed[4] == 1000 && packed[5] == -2000);
}

TEST(finger_curls_reach_every_vrchat_gesture) {
    const struct {
        HandSense in;
        std::array<float, 5> curl;
    } cases[] = {
        {{false, false, 0, 0}, {0, 0, 0, 0, 0}},      // open
        {{false, false, 0, 1}, {0, 0, 1, 1, 1}},      // finger gun
        {{false, true, 1, 1}, {0, 1, 1, 1, 1}},       // thumbs up
        {{true, false, 0, 1}, {0.7f, 0, 1, 1, 1}},    // point
        {{true, true, 1, 1}, {0.7f, 1, 1, 1, 1}},     // fist
        {{true, false, 0, 0}, {0.7f, 0, 0, 1, 1}},    // peace
        {{true, true, 1, 0}, {0.7f, 0, 1, 1, 0}},     // horns
        {{true, true, 0, 0}, {0.7f, 0.25f, 0, 0, 0}}, // resting on the trigger: neutral
    };
    for (const auto& c : cases) {
        const auto curl = FingerCurls(c.in);
        for (size_t i = 0; i < 5; ++i) CHECK_NEAR(curl[i], c.curl[i], 1e-6);
    }
}

TEST(math_normalize_rejects_degenerate) {
    Quat q{0, 0, 0, 0};
    CHECK(!Normalize(q));
    Quat n{std::nan(""), 0, 0, 1};
    CHECK(!Normalize(n));
    Quat s{2, 0, 0, 0};
    CHECK(Normalize(s));
    CHECK_NEAR(s.w, 1.0, 1e-12);
}

TEST(math_rotate_yaw_90_maps_forward_to_left) {
    // SteamVR: -Z forward, +Y up. Yaw +90deg (counter-clockwise seen from above)
    // turns forward (-Z) into left (-X).
    const Quat q = FromAxisAngle({0, 1, 0}, kPi / 2);
    const Vec3 v = Rotate(q, {0, 0, -1});
    CHECK_NEAR(v.x, -1.0, 1e-9);
    CHECK_NEAR(v.y, 0.0, 1e-9);
    CHECK_NEAR(v.z, 0.0, 1e-9);
}

TEST(math_slerp_halfway) {
    const Quat a{};
    const Quat b = FromAxisAngle({0, 1, 0}, kPi / 2);
    const Quat h = Slerp(a, b, 0.5);
    CHECK_NEAR(AngleBetween(h, FromAxisAngle({0, 1, 0}, kPi / 4)), 0.0, 1e-6);
    // Shortest path: -b is the same rotation.
    const Quat nb{-b.w, -b.x, -b.y, -b.z};
    CHECK_NEAR(AngleBetween(Slerp(a, nb, 0.5), h), 0.0, 1e-6);
}

TEST(math_yaw_pitch_roll_composition) {
    const Quat q = FromYawPitchRoll(kPi / 2, 0, 0);
    CHECK_NEAR(AngleBetween(q, FromAxisAngle({0, 1, 0}, kPi / 2)), 0.0, 1e-9);
}

TEST(math_smoother_zero_is_passthrough) {
    PoseSmoother s;
    s.Update({1, 2, 3}, {}, 0.0, 0.011);
    CHECK_NEAR(s.Position().x, 1.0, 1e-12);
    s.Update({2, 2, 3}, {}, 0.0, 0.011);
    CHECK_NEAR(s.Position().x, 2.0, 1e-12);
}

TEST(math_smoother_converges_and_is_rate_independent) {
    PoseSmoother a, b;
    a.Reset({0, 0, 0}, {});
    b.Reset({0, 0, 0}, {});
    // 1 second at 90 Hz vs 1 second at 360 Hz should land at the same point.
    for (int i = 0; i < 90; ++i) a.Update({1, 0, 0}, {}, 0.8, 1.0 / 90);
    for (int i = 0; i < 360; ++i) b.Update({1, 0, 0}, {}, 0.8, 1.0 / 360);
    CHECK_NEAR(a.Position().x, b.Position().x, 1e-6);
    CHECK(a.Position().x > 0.99);
}

TEST(coords_unity_to_driver_mirrors_z) {
    const Vec3 p = UnityToDriver(Vec3{1, 2, 3});
    CHECK_NEAR(p.x, 1, 0);
    CHECK_NEAR(p.y, 2, 0);
    CHECK_NEAR(p.z, -3, 0);
    // A Unity yaw of +90deg (turn right, clockwise from above in a left-handed
    // system) must become a driver yaw of -90deg.
    const Quat unity_yaw = FromAxisAngle({0, 1, 0}, kPi / 2);
    const Quat d = UnityToDriver(unity_yaw);
    CHECK_NEAR(AngleBetween(d, FromAxisAngle({0, 1, 0}, -kPi / 2)), 0.0, 1e-9);
    // Unity forward (+Z) rotated by unity_yaw is +X; in driver space forward
    // (-Z) rotated by d must also be +X.
    const Vec3 f = Rotate(d, {0, 0, -1});
    CHECK_NEAR(f.x, 1.0, 1e-9);
}
