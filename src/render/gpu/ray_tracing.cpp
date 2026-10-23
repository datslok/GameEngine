#include "render/gpu/ray_tracing.h"

#include <cmath>
#include <stdexcept>

std::uint32_t rayMaskFor(const DrawItem& draw) {
    if (isTransparent(draw.material)) {
        return 0;
    }

    if (!draw.castsShadows) {
        return visualOnlyRayMask;
    }

    return draw.isStatic ? solidRayMask | staticRayMask : solidRayMask;
}

GpuRayInstance packRayInstance(const DrawItem& draw, std::uint32_t instanceNumber, std::uint64_t meshStructure, std::uint32_t flags) {
    if (instanceNumber >= maxRayInstances) {
        throw std::out_of_range("packRayInstance: an instance's number must fit in 24 bits");
    }

    GpuRayInstance instance;

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 4; ++column) {
            instance.transform[row * 4 + column] = draw.model.values[row][column];
        }
    }

    instance.indexAndMask = instanceNumber | (rayMaskFor(draw) << 24);
    instance.recordAndFlags = ((flags | (isCutOut(draw.material) ? rayInstanceNotOpaque : 0u)) & 0xFFu) << 24;
    instance.accelerationStructure = meshStructure;
    return instance;
}

// Anticlockwise corners make (b - a) x (c - a) point out of the front, so a ray against that normal meets the front.
bool hitsFrontFace(const Vec3& a, const Vec3& b, const Vec3& c, const Vec3& direction, float handedness) {
    const float facing = (b - a).cross(c - a).dot(direction);
    return handedness < 0.0f ? facing > 0.0f : facing < 0.0f;
}

/*
* The camera's view up is square to its forward and right (its stored up is the world's). Scaled by the tangents of
* half the view's height and width, x = y = 1 reaches the top right corner of the view.
*/
GpuRayDebugView packRayDebugView(const Camera& camera, RayDebugView view, std::uint32_t screenWidth, std::uint32_t screenHeight) {
    const float tanHalfHeight = std::tan(camera.getVerticalFov() * 0.5f);
    const float tanHalfWidth = tanHalfHeight * camera.getAspectRatio();
    const Vec3 right = camera.getRight() * tanHalfWidth;
    const Vec3 up = camera.getRight().cross(camera.getForward()).normalized() * tanHalfHeight;
    const Vec3 forward = camera.getForward();
    const Vec3 origin = camera.getPosition();

    GpuRayDebugView packed{};
    packed.right[0] = right.x;
    packed.right[1] = right.y;
    packed.right[2] = right.z;
    packed.up[0] = up.x;
    packed.up[1] = up.y;
    packed.up[2] = up.z;
    packed.forward[0] = forward.x;
    packed.forward[1] = forward.y;
    packed.forward[2] = forward.z;
    packed.originAndView[0] = origin.x;
    packed.originAndView[1] = origin.y;
    packed.originAndView[2] = origin.z;
    packed.originAndView[3] = static_cast<float>(view);
    packed.screen[0] = static_cast<float>(screenWidth);
    packed.screen[1] = static_cast<float>(screenHeight);
    return packed;
}

Vec3 cameraRayDirection(const Vec3& right, const Vec3& up, const Vec3& forward, float x, float y) {
    return forward + right * x + up * y;
}

Vec3 hitCornerWeights(float second, float third) {
    return Vec3{1.0f - second - third, second, third};
}
