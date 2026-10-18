#include "math/pose_math.h"

#include <algorithm>

namespace vhmd {

bool IsFinite(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

bool IsFinite(const Quat& q) {
    return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
}

double Length(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

double Norm(const Quat& q) { return std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z); }

bool Normalize(Quat& q) {
    if (!IsFinite(q)) return false;
    const double n = Norm(q);
    if (n < 1e-9) return false;
    q.w /= n;
    q.x /= n;
    q.y /= n;
    q.z /= n;
    return true;
}

Quat Multiply(const Quat& a, const Quat& b) {
    return {
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
    };
}

Quat Conjugate(const Quat& q) { return {q.w, -q.x, -q.y, -q.z}; }

Vec3 Rotate(const Quat& q, const Vec3& v) {
    const Quat p{0.0, v.x, v.y, v.z};
    const Quat r = Multiply(Multiply(q, p), Conjugate(q));
    return {r.x, r.y, r.z};
}

Quat FromAxisAngle(const Vec3& axis, double radians) {
    const double len = Length(axis);
    if (len < 1e-12) return {};
    const double s = std::sin(radians * 0.5) / len;
    return {std::cos(radians * 0.5), axis.x * s, axis.y * s, axis.z * s};
}

Quat FromYawPitchRoll(double yaw, double pitch, double roll) {
    const Quat qy = FromAxisAngle({0, 1, 0}, yaw);
    const Quat qx = FromAxisAngle({1, 0, 0}, pitch);
    const Quat qz = FromAxisAngle({0, 0, 1}, roll);
    return Multiply(Multiply(qy, qx), qz);
}

Vec3 Lerp(const Vec3& a, const Vec3& b, double t) { return a + (b - a) * t; }

Quat Slerp(const Quat& a, const Quat& b_in, double t) {
    Quat b = b_in;
    double dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot < 0.0) {
        b = {-b.w, -b.x, -b.y, -b.z};
        dot = -dot;
    }
    if (dot > 0.9995) {
        Quat r{a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
        Normalize(r);
        return r;
    }
    const double theta0 = std::acos(std::clamp(dot, -1.0, 1.0));
    const double theta = theta0 * t;
    const double s0 = std::cos(theta) - dot * std::sin(theta) / std::sin(theta0);
    const double s1 = std::sin(theta) / std::sin(theta0);
    return {a.w * s0 + b.w * s1, a.x * s0 + b.x * s1, a.y * s0 + b.y * s1, a.z * s0 + b.z * s1};
}

double AngleBetween(const Quat& a, const Quat& b) {
    const double dot = std::abs(a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z);
    return 2.0 * std::acos(std::clamp(dot, 0.0, 1.0));
}

void PoseSmoother::Reset(const Vec3& p, const Quat& q) {
    p_ = p;
    q_ = q;
    initialized_ = true;
}

void PoseSmoother::Update(const Vec3& target_p, const Quat& target_q, double smoothing, double dt_seconds) {
    if (!initialized_ || smoothing <= 0.0) {
        Reset(target_p, target_q);
        return;
    }
    smoothing = std::clamp(smoothing, 0.0, 0.999);
    // Convert the per-90Hz-tick retention factor into one for the actual dt.
    const double ticks = std::max(dt_seconds, 0.0) * 90.0;
    const double retain = std::pow(smoothing, ticks);
    const double alpha = 1.0 - retain;
    p_ = Lerp(p_, target_p, alpha);
    q_ = Slerp(q_, target_q, alpha);
}

} // namespace vhmd
