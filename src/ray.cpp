#include "ray.h"

#include <cmath>
#include <stdexcept>

Ray makeCameraRay(
    const Camera& camera,
    float mouseX,
    float mouseY,
    float aspectRatio
) {
    if (!std::isfinite(mouseX) ||
        !std::isfinite(mouseY) ||
        !std::isfinite(aspectRatio) ||
        aspectRatio <= 0.0f) {
        throw std::invalid_argument(
            "Camera ray requires finite coordinates and positive aspect ratio"
        );
    }

    // Convert window coordinates to normalized device coordinates.
    const float ndcX = 2.0f * mouseX - 1.0f;
    const float ndcY = 1.0f - 2.0f * mouseY;

    const Mat4 projection = camera.getProjectionMatrix();

    // For our perspective matrix, this is cot(verticalFov / 2).
    const float verticalScale = projection.values[1][1];

    const float horizontalOffset =
        ndcX * aspectRatio / verticalScale;

    const float verticalOffset =
        ndcY / verticalScale;

    const Vec3 forward = camera.getForward();
    const Vec3 right = camera.getRight();

    // Screen-up is perpendicular to the viewing direction.
    // camera.getUp() is the fixed reference-up axis instead.
    const Vec3 screenUp = right.cross(forward).normalized();

    const Vec3 direction = (
        forward +
        right * horizontalOffset +
        screenUp * verticalOffset
    ).normalized();

    return Ray{
        camera.getPosition(),
        direction
    };
}

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