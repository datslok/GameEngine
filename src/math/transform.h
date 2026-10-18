#pragma once

#include "math/mat4.h"
#include "math/quat.h"
#include "math/vec3.h"

struct Transform {
    Vec3 position{0.0f, 0.0f, 0.0f};
    Quat rotation{}; // The identity: no turn.
    Vec3 scale{1.0f, 1.0f, 1.0f};

    Mat4 getMatrix() const;
};

// Blend between two transforms, with alpha 0 giving from and 1 giving to.
// Rotations blend along the shortest turn between the two orientations (slerp).
Transform interpolate(const Transform& from, const Transform& to, float alpha);