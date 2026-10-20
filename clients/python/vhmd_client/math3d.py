"""Small quaternion helpers. Quaternions are tuples (w, x, y, z).

Coordinate system: SteamVR (right handed, +Y up, -Z forward, meters).
Positive yaw turns to the left (counter-clockwise seen from above).
"""

from __future__ import annotations

import math
from typing import Sequence, Tuple

Quat = Tuple[float, float, float, float]
Vec3 = Tuple[float, float, float]

IDENTITY: Quat = (1.0, 0.0, 0.0, 0.0)


def normalize(q: Sequence[float]) -> Quat:
    n = math.sqrt(sum(c * c for c in q))
    if n < 1e-12:
        return IDENTITY
    return (q[0] / n, q[1] / n, q[2] / n, q[3] / n)


def multiply(a: Sequence[float], b: Sequence[float]) -> Quat:
    aw, ax, ay, az = a
    bw, bx, by, bz = b
    return (
        aw * bw - ax * bx - ay * by - az * bz,
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
    )


def from_axis_angle(axis: Sequence[float], radians: float) -> Quat:
    n = math.sqrt(sum(c * c for c in axis))
    if n < 1e-12:
        return IDENTITY
    s = math.sin(radians / 2) / n
    return (math.cos(radians / 2), axis[0] * s, axis[1] * s, axis[2] * s)


def from_euler(yaw: float = 0.0, pitch: float = 0.0, roll: float = 0.0, degrees: bool = True) -> Quat:
    """Yaw about +Y, then pitch about +X, then roll about +Z (intrinsic)."""
    if degrees:
        yaw, pitch, roll = (math.radians(v) for v in (yaw, pitch, roll))
    q = multiply(from_axis_angle((0, 1, 0), yaw), from_axis_angle((1, 0, 0), pitch))
    return multiply(q, from_axis_angle((0, 0, 1), roll))


def to_euler(q: Sequence[float], degrees: bool = True) -> Vec3:
    """Inverse of from_euler: (yaw, pitch, roll). Near pitch = +-90 degrees yaw
    and roll are not unique; roll is then reported as 0."""
    w, x, y, z = normalize(q)
    r02 = 2 * (x * z + w * y)
    r12 = 2 * (y * z - w * x)
    r22 = 1 - 2 * (x * x + y * y)
    r10 = 2 * (x * y + w * z)
    r11 = 1 - 2 * (x * x + z * z)
    pitch = math.asin(max(-1.0, min(1.0, -r12)))
    if abs(r12) < 0.999999:
        yaw = math.atan2(r02, r22)
        roll = math.atan2(r10, r11)
    else:
        r00 = 1 - 2 * (y * y + z * z)
        r20 = 2 * (x * z - w * y)
        yaw = math.atan2(-r20, r00)
        roll = 0.0
    if degrees:
        return (math.degrees(yaw), math.degrees(pitch), math.degrees(roll))
    return (yaw, pitch, roll)


def conjugate(q: Sequence[float]) -> Quat:
    """Inverse of a unit quaternion."""
    return (q[0], -q[1], -q[2], -q[3])


def rotate(q: Sequence[float], v: Sequence[float]) -> Vec3:
    w, x, y, z = q
    p = (0.0, v[0], v[1], v[2])
    r = multiply(multiply(q, p), (w, -x, -y, -z))
    return (r[1], r[2], r[3])


def angle_between(a: Sequence[float], b: Sequence[float]) -> float:
    """Angle in radians between two orientations."""
    d = abs(sum(x * y for x, y in zip(normalize(a), normalize(b))))
    return 2.0 * math.acos(min(1.0, d))


def from_matrix34(m) -> Tuple[Vec3, Quat]:
    """Convert an OpenVR HmdMatrix34_t (or 3x4 nested list) to (pos, quat)."""
    r = [[m[i][j] for j in range(4)] for i in range(3)]
    tr = r[0][0] + r[1][1] + r[2][2]
    if tr > 0:
        s = math.sqrt(tr + 1.0) * 2
        q = (0.25 * s, (r[2][1] - r[1][2]) / s, (r[0][2] - r[2][0]) / s, (r[1][0] - r[0][1]) / s)
    elif r[0][0] > r[1][1] and r[0][0] > r[2][2]:
        s = math.sqrt(1.0 + r[0][0] - r[1][1] - r[2][2]) * 2
        q = ((r[2][1] - r[1][2]) / s, 0.25 * s, (r[0][1] + r[1][0]) / s, (r[0][2] + r[2][0]) / s)
    elif r[1][1] > r[2][2]:
        s = math.sqrt(1.0 + r[1][1] - r[0][0] - r[2][2]) * 2
        q = ((r[0][2] - r[2][0]) / s, (r[0][1] + r[1][0]) / s, 0.25 * s, (r[1][2] + r[2][1]) / s)
    else:
        s = math.sqrt(1.0 + r[2][2] - r[0][0] - r[1][1]) * 2
        q = ((r[1][0] - r[0][1]) / s, (r[0][2] + r[2][0]) / s, (r[1][2] + r[2][1]) / s, 0.25 * s)
    return (r[0][3], r[1][3], r[2][3]), normalize(q)
