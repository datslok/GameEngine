#pragma once

#include "input/input.h"
#include "math/vec3.h"
#include "scene/camera.h"

#include <optional>

struct MobaCameraSettings {
    // Where the camera sits relative to the point it looks at, which also sets the viewing angle.
    Vec3 offset{0.0f, 12.0f, 10.0f};
    float panSpeed = 9.0f;          // units per second
    float edgeMarginPixels = 5.0f;  // how close to a window edge the cursor must be to pan
};

/*
* A top-down MOBA camera at a fixed angle: it follows a target when given one, and otherwise pans when the confined cursor touches a window edge.
* Whether it follows (for example a lock key) is the game's choice, so the controller keeps no state besides its settings.
*/
class MobaCameraController {
public:
    explicit MobaCameraController(const MobaCameraSettings& settings = {});

    // Place the camera so it looks at a point.
    void centreOn(Camera& camera, const Vec3& point) const;

    // The point the camera looks at (on the ground, when centred on ground points), the reverse of centreOn.
    Vec3 getLookPoint(const Camera& camera) const;

    void update(Camera& camera, const Input& input, float frameSeconds,
                const std::optional<Vec3>& followTarget) const;

    // Read every frame. After changing the offset, centre the camera again so getLookPoint stays right.
    MobaCameraSettings& getSettings();

private:
    MobaCameraSettings settings;
};
