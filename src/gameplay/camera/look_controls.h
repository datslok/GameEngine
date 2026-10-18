#pragma once

#include "core/settings_visitor.h"
#include "input/input.h"
#include "math/angles.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "scene/camera.h"

/*
* A view direction stored as angles, like spherical coordinates with world up = +Y.
* yaw 0 looks down -Z and positive yaw turns right (towards +X); pitch 0 is level and positive pitch looks up.
* Keeping angles, instead of a direction vector, makes the pitch limit exact and is what an FPS sends over the network.
*/
struct ViewAngles {
    float yaw = 0.0f;
    float pitch = 0.0f;
};

// The tuning shared by every mouse-look controller.
struct LookSettings {
    float radiansPerPixel = 0.001f; // mouse sensitivity
    float sprintMultiplier = 2.0f;  // movement speed multiple while either Shift key is held (run, or fly fast)
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(LookSettings& settings, SettingsVisitor& visitor);

// Stop just short of straight up or down, where yaw would become meaningless.
inline constexpr float maxPitchRadians = radiansFromDegrees(89.0f);

// The unit direction these angles point along.
Vec3 directionFromAngles(const ViewAngles& angles);

// The angles of a direction, so a controller can take over from wherever the camera currently looks.
ViewAngles anglesFromDirection(const Vec3& direction);

// Turn by mouse motion: moving right turns right, moving up looks up. Pitch is clamped to +/- maxPitchRadians.
void applyMouseLook(ViewAngles& angles, const Vec2& mouseDelta, float radiansPerPixel);

// WASD as axes: x is strafe (+1 right), y is forward (+1 forward). Not normalized.
Vec2 getMoveAxes(const Input& input);

// The factor to scale movement speed by this frame: sprintMultiplier while Shift is held, otherwise 1.
float getSpeedMultiplier(const Input& input, float sprintMultiplier);

// Turn the angles by this frame's mouse motion and point the camera along them.
void turnCamera(Camera& camera, ViewAngles& angles, const Input& input, const LookSettings& look);

// Move the camera along a direction at speed (times the sprint multiplier while Shift is held). The direction is
// normalized first, so moving diagonally is no faster; a zero direction does nothing.
void moveCamera(Camera& camera, const Vec3& direction, float speed, const Input& input, const LookSettings& look, float frameSeconds);
