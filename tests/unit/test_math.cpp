#include "test_framework.h"

#include "math/pose_math.h"
#include "protocol/osc.h"

using namespace vhmd;

namespace {
constexpr double kPi = 3.14159265358979323846;
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
