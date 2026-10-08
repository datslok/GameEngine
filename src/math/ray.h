#pragma once

#include "math/vec3.h"

#include <optional>

struct Ray {
    Vec3 origin;
    Vec3 direction;
};

// Find the forward intersection with a horizontal plane.
std::optional<Vec3> intersectGround(
    const Ray& ray,
    float groundHeight = 0.0f
);