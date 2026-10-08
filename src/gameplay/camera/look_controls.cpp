#include "gameplay/camera/look_controls.h"

#include <algorithm>
#include <cmath>
#include <numbers>

/*
* Spherical to Cartesian: the horizontal part has length cos(pitch), split between x and -z by the yaw.
*/
Vec3 directionFromAngles(const ViewAngles& angles) {
    const float horizontal = std::cos(angles.pitch);

    return Vec3{
        horizontal * std::sin(angles.yaw),
        std::sin(angles.pitch),
        -horizontal * std::cos(angles.yaw)
    };
}

/*
* Cartesian to spherical, the inverse of directionFromAngles. atan2 keeps the right quadrant for every heading.
*/
ViewAngles anglesFromDirection(const Vec3& direction) {
    const Vec3 unit = direction.normalized();

    return ViewAngles{
        std::atan2(unit.x, -unit.z),
        std::asin(std::clamp(unit.y, -1.0f, 1.0f))
    };
}

void applyMouseLook(ViewAngles& angles, const Vec2& mouseDelta, float radiansPerPixel) {
    angles.yaw = std::remainder(angles.yaw + mouseDelta.x * radiansPerPixel, 2.0f * std::numbers::pi_v<float>);

    // Screen y grows downwards, so moving the mouse up (negative y) looks up.
    angles.pitch = std::clamp(
        angles.pitch - mouseDelta.y * radiansPerPixel,
        -maxPitchRadians,
        maxPitchRadians
    );
}

Vec2 getMoveAxes(const Input& input) {
    Vec2 axes{};

    if (input.isKeyHeld(Key::W)) {
        axes.y += 1.0f;
    }

    if (input.isKeyHeld(Key::S)) {
        axes.y -= 1.0f;
    }

    if (input.isKeyHeld(Key::D)) {
        axes.x += 1.0f;
    }

    if (input.isKeyHeld(Key::A)) {
        axes.x -= 1.0f;
    }

    return axes;
}
