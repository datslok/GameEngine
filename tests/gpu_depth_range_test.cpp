#include "math/mat4.h"
#include "render/gpu/gpu_depth_range.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    constexpr float nearPlane = 0.5f;
    constexpr float farPlane = 100.0f;

    // Depth after the GPU's divide by w, for a point straight ahead of the camera at the given distance.
    float gpuDepthAt(const Mat4& projection, float distance) {
        const Vec4 clip = projection * Vec4{0.0f, 0.0f, -distance, 1.0f};
        return clip.z / clip.w;
    }
}

/*
* Our projection follows OpenGL (depth -1..1); Vulkan wants 0..1. The near plane must land on 0 and the far plane on 1.
*/
void testGpuDepthRange() {
    const Mat4 projection = Mat4::perspective(1.0f, 1.5f, nearPlane, farPlane);
    const Mat4 gpu = toGpuDepthRange(projection);

    assert(nearlyEqual(gpuDepthAt(projection, nearPlane), -1.0f));
    assert(nearlyEqual(gpuDepthAt(gpu, nearPlane), 0.0f));
    assert(nearlyEqual(gpuDepthAt(gpu, farPlane), 1.0f));

    const float middle = gpuDepthAt(gpu, 10.0f);
    assert(middle > 0.0f && middle < 1.0f);

    // Only depth changes: x, y and w are exactly what the projection gave.
    const Vec4 point{1.0f, 2.0f, -10.0f, 1.0f};
    const Vec4 before = projection * point;
    const Vec4 after = gpu * point;
    assert(nearlyEqual(after.x, before.x));
    assert(nearlyEqual(after.y, before.y));
    assert(nearlyEqual(after.w, before.w));
}
