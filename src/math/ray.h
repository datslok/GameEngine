#pragma once

#include "scene/camera.h"
#include "math/vec3.h"

#include <optional>

struct Ray {
    Vec3 origin;
    Vec3 direction;
};

// Mouse coordinates are normalized:
// (0, 0) = top-left, (1, 1) = bottom-right.
Ray makeCameraRay(
    const Camera& camera,
    float mouseX,
    float mouseY,
    float aspectRatio
);

// Find the forward intersection with a horizontal plane.
std::optional<Vec3> intersectGround(
    const Ray& ray,
    float groundHeight = 0.0f
);
