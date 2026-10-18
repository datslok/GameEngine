#include "math/mat4.h"
#include "render/gpu/gpu_depth_range.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>

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

    // How many representable floats lie between two positive floats: for positive floats, the bit patterns count up in
    // the same order as the values, one step per float.
    std::int64_t floatStepsBetween(float a, float b) {
        const std::int64_t difference = static_cast<std::int64_t>(std::bit_cast<std::int32_t>(a)) - std::bit_cast<std::int32_t>(b);
        return difference < 0 ? -difference : difference;
    }

    // The old convention, near at 0 and far at 1, kept here only to compare against.
    Mat4 nearAtZero(const Mat4& projection) {
        Mat4 correction = Mat4::identity();
        correction.values[2][2] = 0.5f;
        correction.values[2][3] = 0.5f;
        return correction * projection;
    }

    /*
    * Reversed-Z: our projection follows OpenGL (depth -1..1), the GPU keeps 0..1, and the conversion turns the range
    * round, so the near plane lands on 1 and the far plane on 0.
    */
    void testNearPlaneAtOneFarPlaneAtZero() {
        const Mat4 projection = Mat4::perspective(1.0f, 1.5f, nearPlane, farPlane);
        const Mat4 gpu = toGpuDepthRange(projection);

        assert(nearlyEqual(gpuDepthAt(projection, nearPlane), -1.0f));
        assert(nearlyEqual(gpuDepthAt(gpu, nearPlane), gpuNearDepth));
        assert(nearlyEqual(gpuDepthAt(gpu, farPlane), gpuFarDepth));
        assert(gpuNearDepth == 1.0f && gpuFarDepth == 0.0f);

        // Nearer is larger, all the way through, which is why the depth test keeps the greater value.
        float previous = 2.0f;

        for (float distance : {0.5f, 1.0f, 3.0f, 10.0f, 40.0f, 99.0f}) {
            const float depth = gpuDepthAt(gpu, distance);
            assert(depth > 0.0f - 1e-6f && depth < previous);
            previous = depth;
        }

        // Only depth changes: x, y and w are exactly what the projection gave.
        const Vec4 point{1.0f, 2.0f, -10.0f, 1.0f};
        const Vec4 before = projection * point;
        const Vec4 after = gpu * point;
        assert(nearlyEqual(after.x, before.x));
        assert(nearlyEqual(after.y, before.y));
        assert(nearlyEqual(after.w, before.w));
    }

    // A box-shaped view (a directional light's shadow) turns round the same way: its near side at 1, its far side at 0.
    void testOrthographicIsReversedToo() {
        const Mat4 box = toGpuDepthRange(Mat4::orthographic(-5.0f, 5.0f, -5.0f, 5.0f, 2.0f, 30.0f));

        assert(nearlyEqual(gpuDepthAt(box, 2.0f), gpuNearDepth));
        assert(nearlyEqual(gpuDepthAt(box, 30.0f), gpuFarDepth));
        assert(nearlyEqual(gpuDepthAt(box, 16.0f), 0.5f));
    }

    /*
    * Why reversed-Z. Perspective depth goes as 1/distance, so far away it barely changes: two surfaces 1 cm apart
    * 1 km away differ in depth by only about 1e-9. With the far plane at 1 (the old way), depths out there sit just
    * below 1, where neighbouring floats are 6e-8 apart, so both surfaces get the same depth and flicker through each
    * other (z-fighting). With the far plane at 0 they sit near 1e-4, where floats are about 7e-12 apart: over a hundred
    * floats between them. The divide by w is done in floats, as the GPU does it.
    */
    void testDistantSurfacesStayApart() {
        const Mat4 projection = Mat4::perspective(1.2f, 1.0f, 0.1f, 2000.0f);
        const float front = 1000.0f;
        const float behind = 1000.01f;

        const std::int64_t reversedSteps = floatStepsBetween(gpuDepthAt(toGpuDepthRange(projection), front),
                                                             gpuDepthAt(toGpuDepthRange(projection), behind));
        const std::int64_t oldSteps = floatStepsBetween(gpuDepthAt(nearAtZero(projection), front),
                                                        gpuDepthAt(nearAtZero(projection), behind));

        assert(reversedSteps >= 50);
        assert(oldSteps <= 1);
    }
}

void testGpuDepthRange() {
    testNearPlaneAtOneFarPlaneAtZero();
    testOrthographicIsReversedToo();
    testDistantSurfacesStayApart();
}
