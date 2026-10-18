// Minimal vector / quaternion math used by the driver core.
// Coordinate system: SteamVR (right handed, +Y up, -Z forward, meters).
// Quaternions are stored and transmitted as [w, x, y, z].
#pragma once

#include <array>
#include <cmath>

namespace vhmd {

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};

struct Quat {
    double w = 1.0, x = 0.0, y = 0.0, z = 0.0;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(const Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }

bool IsFinite(const Vec3& v);
bool IsFinite(const Quat& q);
double Length(const Vec3& v);
double Norm(const Quat& q);

// Returns false (and leaves `q` untouched) if the quaternion is degenerate.
bool Normalize(Quat& q);

Quat Multiply(const Quat& a, const Quat& b);
Quat Conjugate(const Quat& q);
Vec3 Rotate(const Quat& q, const Vec3& v);
Quat FromAxisAngle(const Vec3& axis, double radians);
// Intrinsic Y (yaw) -> X (pitch) -> Z (roll), radians.
Quat FromYawPitchRoll(double yaw, double pitch, double roll);

Vec3 Lerp(const Vec3& a, const Vec3& b, double t);
// Shortest-path spherical interpolation. Inputs must be normalized.
Quat Slerp(const Quat& a, const Quat& b, double t);

// Angle between two orientations in radians (0..pi).
double AngleBetween(const Quat& a, const Quat& b);

// Exponential smoothing helper. `smoothing` in [0,1): 0 = no smoothing
// (output follows input), values close to 1 = heavy smoothing. The filter is
// frame-rate independent: `smoothing` is defined for a 90 Hz reference tick.
class PoseSmoother {
public:
    void Reset(const Vec3& p, const Quat& q);
    void Update(const Vec3& target_p, const Quat& target_q, double smoothing, double dt_seconds);
    const Vec3& Position() const { return p_; }
    const Quat& Rotation() const { return q_; }
    bool Initialized() const { return initialized_; }

private:
    bool initialized_ = false;
    Vec3 p_{};
    Quat q_{};
};

} // namespace vhmd
