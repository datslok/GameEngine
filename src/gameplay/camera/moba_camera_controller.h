#pragma once

#include "input/input.h"
#include "math/vec3.h"
#include "scene/camera.h"

#include <optional>

/*
* A top-down MOBA camera at a fixed angle: it follows a target when given one, and otherwise pans when the confined cursor touches a window edge.
* Whether it follows (for example a lock key) is the game's choice, so the controller itself keeps no state.
*/
class MobaCameraController {
public:
    // offset is where the camera sits relative to the point it looks at, which also sets the viewing angle.
    MobaCameraController(const Vec3& offset, float panSpeed, float edgeMarginPixels = 5.0f);

    // Place the camera so it looks at a point.
    void centreOn(Camera& camera, const Vec3& point) const;

    // The point the camera looks at (on the ground, when centred on ground points), the reverse of centreOn.
    Vec3 getLookPoint(const Camera& camera) const;

    void update(Camera& camera, const Input& input, float frameSeconds,
                const std::optional<Vec3>& followTarget) const;

private:
    Vec3 offset;
    float panSpeed;
    float edgeMarginPixels;
};
