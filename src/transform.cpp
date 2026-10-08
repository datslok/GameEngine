#include "transform.h"

#include <cmath>
#include <numbers>

namespace {
    Vec3 lerp(const Vec3& from, const Vec3& to, float alpha) {
        return from + (to - from) * alpha;
    }

    // remainder() maps the difference into -pi..pi, which is the shortest turn between the two angles.
    float lerpAngle(float from, float to, float alpha) {
        constexpr float fullTurn = 2.0f * std::numbers::pi_v<float>;
        return from + std::remainder(to - from, fullTurn) * alpha;
    }
}

Mat4 Transform::getMatrix() const {
    return
        Mat4::translation(position.x, position.y, position.z) *
        Mat4::rotationZ(rotation.z) *
        Mat4::rotationY(rotation.y) *
        Mat4::rotationX(rotation.x) *
        Mat4::scaling(scale.x, scale.y, scale.z);
}

/*
* Used to draw objects between two simulation ticks, so motion looks smooth at any frame rate.
*/
Transform interpolate(const Transform& from, const Transform& to, float alpha) {
    Transform result;

    result.position = lerp(from.position, to.position, alpha);
    result.rotation = Vec3{
        lerpAngle(from.rotation.x, to.rotation.x, alpha),
        lerpAngle(from.rotation.y, to.rotation.y, alpha),
        lerpAngle(from.rotation.z, to.rotation.z, alpha)
    };
    result.scale = lerp(from.scale, to.scale, alpha);

    return result;
}