#include "render/gpu/stars.h"

#include <algorithm>
#include <cmath>

std::vector<GpuStar> packStars(std::span<const Star> stars) {
    std::vector<GpuStar> packed;
    packed.reserve(stars.size());

    for (const Star& star : stars) {
        packed.push_back(GpuStar{{star.direction.x, star.direction.y, star.direction.z}, star.magnitude, {star.colour.x, star.colour.y, star.colour.z}, 0.0f});
    }

    return packed;
}

GpuStarView packStarView(const GpuRayDebugView& camera, const StarrySky& sky) {
    GpuStarView view{};
    view.camera = camera;

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            view.toWorld[row][column] = sky.toWorld.values[row][column];
        }
    }

    view.toWorld[0][3] = sky.brightness;
    view.toWorld[1][3] = sky.contrast;
    return view;
}

float starLight(float magnitude, float brightness, float contrast) {
    return brightness * std::pow(10.0f, -0.4f * contrast * magnitude);
}

Vec2 starScreenPosition(const Vec3& right, const Vec3& up, const Vec3& forward, const Vec3& direction) {
    const float along = direction.dot(forward);
    return Vec2{direction.dot(right) / (right.lengthSquared() * along), direction.dot(up) / (up.lengthSquared() * along)};
}

float starSplatWeight(const Vec2& offset) {
    return std::max(1.0f - std::abs(offset.x), 0.0f) * std::max(1.0f - std::abs(offset.y), 0.0f);
}

float pixelSolidAngle(const Vec3& right, const Vec3& up, float width, float height, float cosOffAxis) {
    return (2.0f * right.length() / width) * (2.0f * up.length() / height) * cosOffAxis * cosOffAxis * cosOffAxis;
}
