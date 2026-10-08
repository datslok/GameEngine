#include "math/ray.h"

#include <cmath>

std::optional<Vec3> intersectGround(
    const Ray& ray,
    float groundHeight
) {
    // Parallel, or nearly parallel, to the ground.
    if (std::abs(ray.direction.y) < 0.000001f) {
        return std::nullopt;
    }

    const float distance =
        (groundHeight - ray.origin.y) / ray.direction.y;

    // The intersection must be in front of the ray origin.
    if (!std::isfinite(distance) || distance < 0.0f) {
        return std::nullopt;
    }

    Vec3 point = ray.origin + ray.direction * distance;

    if (!std::isfinite(point.x) ||
        !std::isfinite(point.y) ||
        !std::isfinite(point.z)) {
        return std::nullopt;
    }

    point.y = groundHeight;

    return point;
}