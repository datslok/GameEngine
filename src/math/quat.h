#pragma once

#include "math/mat4.h"
#include "math/vec3.h"

#include <cmath>

/*
* A rotation in 3D as a unit quaternion: a scalar part w and a vector part (x, y, z), stored in glTF's order (x, y, z, w)
* so node rotations copy straight in.
*
* A turn by theta about a unit axis n is q = (cos(theta / 2), n sin(theta / 2)), and it rotates a vector v as q v q*
* (v written as the quaternion (0, v), q* the conjugate). The half angle means q and -q are the same rotation: unit
* quaternions cover the rotations twice. Unlike three Euler angles, they never lose an axis (gimbal lock), compose by
* multiplication, and blend along the true shortest turn (slerp).
*
* Every function is defined here in the header so optimised builds can inline it, like the vectors.
*/
struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;

    // The identity, no turn, so a rotation always starts defined.
    Quat() = default;
    // Components in storage order; the result is a rotation only if it has unit length.
    Quat(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}

    // A turn by radians about a unit-length axis (right-handed: counter-clockwise looking down the axis at the origin).
    static Quat fromAxisAngle(const Vec3& unitAxis, float radians) {
        const float halfAngle = radians * 0.5f;
        const float sine = std::sin(halfAngle);
        return Quat{unitAxis.x * sine, unitAxis.y * sine, unitAxis.z * sine, std::cos(halfAngle)};
    }

    // Euler angles in radians in the engine's old order: about x first, then y, then z (the matrix Rz Ry Rx). For
    // describing a fixed or spinning pose in angles; anything that turns or blends should stay a quaternion.
    static Quat fromEulerAngles(const Vec3& radians) {
        return fromAxisAngle(Vec3{0.0f, 0.0f, 1.0f}, radians.z) *
               fromAxisAngle(Vec3{0.0f, 1.0f, 0.0f}, radians.y) *
               fromAxisAngle(Vec3{1.0f, 0.0f, 0.0f}, radians.x);
    }

    /*
    * The exp map: the turn whose axis is the vector's direction and whose angle is its length. It undoes
    * toRotationVector, and turns an angular velocity times a time step into the turn made in that step.
    */
    static Quat fromRotationVector(const Vec3& rotation) {
        const float angle = rotation.length();
        if (angle == 0.0f) {
            return Quat{};
        }
        // sin(angle / 2) / angle keeps its precision for tiny angles, unlike dividing the vector by its length first.
        const float scale = std::sin(angle * 0.5f) / angle;
        return Quat{rotation.x * scale, rotation.y * scale, rotation.z * scale, std::cos(angle * 0.5f)};
    }

    /*
    * The Hamilton product: this * other turns by other first, then by this, the same order as matrices multiplying
    * column vectors. In scalar and vector parts: (w1 w2 - v1 . v2, w1 v2 + w2 v1 + v1 x v2); the cross product is why
    * the order matters.
    */
    Quat operator*(const Quat& other) const {
        return Quat{
            w * other.x + other.w * x + (y * other.z - z * other.y),
            w * other.y + other.w * y + (z * other.x - x * other.z),
            w * other.z + other.w * z + (x * other.y - y * other.x),
            w * other.w - (x * other.x + y * other.y + z * other.z)
        };
    }

    // Four-component arithmetic, for blending and repairing quaternions; the results are not unit length in general.
    Quat operator*(float scalar) const { return Quat{x * scalar, y * scalar, z * scalar, w * scalar}; }
    Quat operator+(const Quat& other) const { return Quat{x + other.x, y + other.y, z + other.z, w + other.w}; }
    Quat operator-(const Quat& other) const { return Quat{x - other.x, y - other.y, z - other.z, w - other.w}; }
    // The same rotation, from the other side of the double cover.
    Quat operator-() const { return Quat{-x, -y, -z, -w}; }

    // The vector part flipped: for a unit quaternion, the opposite turn.
    Quat conjugate() const { return Quat{-x, -y, -z, w}; }

    // The 4D dot product: the cosine of half the angle between two unit quaternions (negative across the double cover).
    float dot(const Quat& other) const { return x * other.x + y * other.y + z * other.z + w * other.w; }
    float length() const { return std::sqrt(dot(*this)); }

    // Back to unit length. Rounding in long chains of products slowly stretches a quaternion, and only unit ones are
    // rotations.
    Quat normalized() const { return *this * (1.0f / length()); }

    /*
    * q v q* expanded and simplified for a unit quaternion with vector part u: v + w t + u x t, with t = 2 (u x v).
    * Two cross products instead of two full quaternion products.
    */
    Vec3 rotate(const Vec3& vector) const {
        const Vec3 axisPart{x, y, z};
        const Vec3 twiceCross = axisPart.cross(vector) * 2.0f;
        return vector + twiceCross * w + axisPart.cross(twiceCross);
    }

    // The rotation matrix of a unit quaternion, read straight from its components with no trigonometry.
    Mat4 toMatrix() const {
        Mat4 result;
        result.values[0][0] = 1.0f - 2.0f * (y * y + z * z);
        result.values[0][1] = 2.0f * (x * y - w * z);
        result.values[0][2] = 2.0f * (x * z + w * y);
        result.values[1][0] = 2.0f * (x * y + w * z);
        result.values[1][1] = 1.0f - 2.0f * (x * x + z * z);
        result.values[1][2] = 2.0f * (y * z - w * x);
        result.values[2][0] = 2.0f * (x * z - w * y);
        result.values[2][1] = 2.0f * (y * z + w * x);
        result.values[2][2] = 1.0f - 2.0f * (x * x + y * y);
        result.values[3][3] = 1.0f;
        return result;
    }

    /*
    * The log map (doubled): the turn as one vector, its axis times its angle, always the shorter way (angle 0..pi). For
    * the error between two orientations, (target * current.conjugate()).toRotationVector() is what is left to turn, in
    * world axes, which a joint drive can treat like a displacement for a spring.
    */
    Vec3 toRotationVector() const {
        // Pick the side of the double cover with w >= 0, the turn of at most pi.
        const float sign = w < 0.0f ? -1.0f : 1.0f;
        const Vec3 axisPart{x * sign, y * sign, z * sign};
        const float sineOfHalf = axisPart.length();
        if (sineOfHalf == 0.0f) {
            return Vec3{};
        }
        // atan2 stays accurate at every angle, where acos(w) loses precision for small turns.
        const float angle = 2.0f * std::atan2(sineOfHalf, w * sign);
        return axisPart * (angle / sineOfHalf);
    }
};

/*
* The angle of the smallest turn from one orientation to the other, 0..pi. Uses atan2 on the turn between them, which stays
* precise for nearly equal orientations, where acos of their dot product would not.
*/
inline float angleBetween(const Quat& from, const Quat& to) {
    const Quat difference = to * from.conjugate();
    const float sineOfHalf = std::sqrt(difference.x * difference.x + difference.y * difference.y + difference.z * difference.z);
    return 2.0f * std::atan2(sineOfHalf, std::abs(difference.w));
}

// Above this dot product (about 1.8 degrees apart) slerp blends linearly instead: sin(angle) would be nearly zero, and over
// so small an arc the straight blend, once normalised, is off the constant-speed path by far less than float precision.
inline constexpr float slerpLinearThreshold = 0.9995f;

/*
* Spherical linear interpolation: alpha 0 gives from, 1 gives to, and in between it moves along the shortest great circle
* on the sphere of unit quaternions at constant angular speed, so the blend is the true in-between orientation. When the
* two are on opposite sides of the double cover, to is flipped first so the turn takes the short way round.
*/
inline Quat slerp(const Quat& from, Quat to, float alpha) {
    float cosine = from.dot(to);
    if (cosine < 0.0f) {
        to = -to;
        cosine = -cosine;
    }

    if (cosine > slerpLinearThreshold) {
        return (from + (to - from) * alpha).normalized();
    }

    const float angle = std::acos(cosine);
    const float sine = std::sin(angle);
    return from * (std::sin((1.0f - alpha) * angle) / sine) + to * (std::sin(alpha * angle) / sine);
}
