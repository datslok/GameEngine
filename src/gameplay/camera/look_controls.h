#pragma once

#include "input/input.h"
#include "math/vec2.h"
#include "math/vec3.h"

#include <numbers>

/*
* A view direction stored as angles, like spherical coordinates with world up = +Y.
* yaw 0 looks down -Z and positive yaw turns right (towards +X); pitch 0 is level and positive pitch looks up.
* Keeping angles, instead of a direction vector, makes the pitch limit exact and is what an FPS sends over the network.
*/
struct ViewAngles {
    float yaw = 0.0f;
    float pitch = 0.0f;
};

// Stop just short of straight up or down, where yaw would become meaningless.
inline constexpr float maxPitchRadians = 89.0f * std::numbers::pi_v<float> / 180.0f;

// The unit direction these angles point along.
Vec3 directionFromAngles(const ViewAngles& angles);

// The angles of a direction, so a controller can take over from wherever the camera currently looks.
ViewAngles anglesFromDirection(const Vec3& direction);

// Turn by mouse motion: moving right turns right, moving up looks up. Pitch is clamped to +/- maxPitchRadians.
void applyMouseLook(ViewAngles& angles, const Vec2& mouseDelta, float radiansPerPixel);

// WASD as axes: x is strafe (+1 right), y is forward (+1 forward). Not normalized.
Vec2 getMoveAxes(const Input& input);
