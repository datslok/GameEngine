#include "render/gpu/depth_range.h"
#include "render/gpu/depth_shared.h"
#include "render/gpu/gpu_depth_range.h"
#include "scene/camera.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <optional>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance) {
        return std::abs(actual - expected) <= tolerance;
    }

    // The depth the GPU stores for a point at this distance straight ahead of the camera (reversed-Z: near 1, far 0).
    float storedDepth(const Camera& camera, float distance) {
        const Vec3 point = camera.getPosition() + camera.getForward() * distance;
        const Mat4 viewProjection = toGpuDepthRange(camera.getProjectionMatrix()) * camera.getViewMatrix();
        const Vec4 clip = viewProjection * Vec4{point.x, point.y, point.z, 1.0f};
        return clip.z / clip.w;
    }

    // Nothing included yet: no surfaces, and both ends at their starting values (any depth replaces them).
    void testAnEmptyRangeHasNoSurfaces() {
        const ScreenDepthRange empty;

        assert(!hasSurfaces(empty));
        assert(empty.farthestBits == noSurfaceFarthestBits);
        assert(empty.nearestBits == 0);
    }

    // The far plane's depth is the sky, where nothing was drawn: it is never a surface, so it never widens the range.
    void testTheSkyIsNotASurface() {
        assert(!isSurfaceDepth(gpuFarDepth));
        assert(isSurfaceDepth(1e-30f));
        assert(isSurfaceDepth(0.5f));
        assert(isSurfaceDepth(gpuNearDepth));

        const ScreenDepthRange sky = includeDepth(includeDepth(ScreenDepthRange{}, gpuFarDepth), gpuFarDepth);
        assert(!hasSurfaces(sky));
    }

    /*
    * With reversed-Z the farthest surface has the smallest depth and the nearest the largest. Depths are positive
    * floats, whose bits as unsigned integers sort in the same order as the numbers, so the GPU can keep both ends with
    * integer atomics; the range here is kept the same way.
    */
    void testTheRangeKeepsBothEnds() {
        const std::vector<float> depths{0.31f, 0.002f, 0.75f, 0.0f, 0.5f, 1e-7f, 0.9f};
        ScreenDepthRange range;

        for (float depth : depths) {
            range = includeDepth(range, depth);
        }

        assert(hasSurfaces(range));
        assert(farthestSurfaceDepth(range) == 1e-7f);
        assert(nearestSurfaceDepth(range) == 0.9f);
        assert(range.farthestBits == std::bit_cast<std::uint32_t>(1e-7f));
    }

    // Two ranges combined are the range of everything in both, in either order, and an empty one changes nothing.
    void testRangesCombine() {
        const ScreenDepthRange left = includeDepth(includeDepth(ScreenDepthRange{}, 0.2f), 0.4f);
        const ScreenDepthRange right = includeDepth(includeDepth(ScreenDepthRange{}, 0.05f), 0.3f);
        const ScreenDepthRange both = combineDepthRanges(left, right);

        assert(farthestSurfaceDepth(both) == 0.05f && nearestSurfaceDepth(both) == 0.4f);

        const ScreenDepthRange swapped = combineDepthRanges(right, left);
        assert(swapped.farthestBits == both.farthestBits && swapped.nearestBits == both.nearestBits);

        const ScreenDepthRange withEmpty = combineDepthRanges(left, ScreenDepthRange{});
        assert(withEmpty.farthestBits == left.farthestBits && withEmpty.nearestBits == left.nearestBits);
    }

    // The farthest surface's depth turns back into its distance along the view, for the camera that drew it.
    void testTheFarthestSurfaceGivesItsDistance() {
        const Camera camera{Vec3{3.0f, 1.7f, -2.0f}, Vec3{-5.0f, 0.5f, -20.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.2217f, 16.0f / 9.0f, 0.1f, 1000.0f};

        for (float distance : {0.5f, 7.0f, 37.0f, 180.0f, 900.0f}) {
            ScreenDepthRange range = includeDepth(ScreenDepthRange{}, storedDepth(camera, distance));
            range = includeDepth(range, storedDepth(camera, 0.3f));

            const std::optional<float> farthest = farthestSurfaceDistance(range, camera.getNearPlane(), camera.getFarPlane());
            assert(farthest.has_value());
            assert(nearlyEqual(*farthest, distance, distance * 1e-3f));
        }

        assert(!farthestSurfaceDistance(ScreenDepthRange{}, camera.getNearPlane(), camera.getFarPlane()).has_value());
    }
}

void testDepthRange() {
    testAnEmptyRangeHasNoSurfaces();
    testTheSkyIsNotASurface();
    testTheRangeKeepsBothEnds();
    testRangesCombine();
    testTheFarthestSurfaceGivesItsDistance();
}
