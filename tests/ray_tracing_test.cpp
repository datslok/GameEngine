#include "render/gpu/ray_tracing.h"
#include "math/mat4.h"
#include "scene/frame_description.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace {
    DrawItem drawAt(const Mat4& model, bool castsShadows = true) {
        DrawItem draw;
        draw.model = model;
        draw.castsShadows = castsShadows;
        return draw;
    }

    // Shadow casters are what blocks light, so global illumination's rays see them; the rest (the glowing faces of
    // lights: the sun disc, the moon, lamp bulbs) only rays that want everything drawn see. The bits do not overlap.
    void testMasksFollowShadowCasting() {
        assert(rayMaskFor(drawAt(Mat4::identity(), true)) == solidRayMask);
        assert(rayMaskFor(drawAt(Mat4::identity(), false)) == visualOnlyRayMask);
        assert((solidRayMask & visualOnlyRayMask) == 0);
        assert(solidRayMask != 0 && visualOnlyRayMask != 0);
        assert((everythingRayMask & solidRayMask) == solidRayMask && (everythingRayMask & visualOnlyRayMask) == visualOnlyRayMask);
    }

    /*
    * The instance as the GPU's top-level structure reads it: the model matrix's top three rows (row-major, the last
    * row of an affine matrix is always 0 0 0 1), then the instance number in the low 24 bits of a word with the mask in
    * the top 8, then the shader record offset (unused by ray queries, 0) with the flags in the top 8, then the mesh's
    * structure's GPU address.
    */
    void testInstancesArePackedForTheGpu() {
        const Mat4 model = Mat4::translation(1.0f, 2.0f, 3.0f) * Mat4::rotationY(0.4f) * Mat4::scaling(2.0f, 0.5f, 1.5f);
        const GpuRayInstance packed = packRayInstance(drawAt(model, false), 12345, 0x1234'5678'9ABC'DEF0ull, 0x2);

        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 4; ++column) {
                assert(packed.transform[row * 4 + column] == model.values[row][column]);
            }
        }

        assert(packed.indexAndMask == (12345u | (visualOnlyRayMask << 24)));
        assert(packed.recordAndFlags == (0x2u << 24));
        assert(packed.accelerationStructure == 0x1234'5678'9ABC'DEF0ull);

        // The instance number must fit in its 24 bits.
        assert(packRayInstance(drawAt(model), maxRayInstances - 1, 1).indexAndMask == ((maxRayInstances - 1) | (solidRayMask << 24)));
        bool threw = false;

        try {
            packRayInstance(drawAt(model), maxRayInstances, 1);
        }
        catch (const std::out_of_range&) {
            threw = true;
        }

        assert(threw);

        // A cut-out is not opaque to rays: every hit on it is a candidate the shader tests against its alpha first.
        DrawItem fence = drawAt(model);
        fence.material.alphaMode = AlphaMode::Mask;
        assert(packRayInstance(fence, 3, 1, 0x2).recordAndFlags == ((0x2u | rayInstanceNotOpaque) << 24));
        assert(packRayInstance(drawAt(model), 3, 1).recordAndFlags == 0);

        // Glass and blended surfaces are not in any ray's world for now: probe rays, shadow rays and reflections pass
        // through them.
        DrawItem window = drawAt(model);
        window.material.transmission = 1.0f;
        window.isStatic = true;
        assert(rayMaskFor(window) == 0);
        assert((packRayInstance(window, 3, 1).indexAndMask >> 24) == 0);
    }

    /*
    * A triangle's front is the side its corners turn anticlockwise when seen from (the engine's winding, so a closed
    * mesh's front faces outwards). A ray hits the front when it travels against the front's normal. A mirrored object's
    * corners turn the other way in the world, but its outside is still its outside, so the handedness flips the answer
    * back: the GPU decides facing in the object's own coordinates, which gives the same.
    */
    void testFrontFacesAreTheWindingsSide() {
        const Vec3 a{0.0f, 0.0f, 0.0f};
        const Vec3 b{1.0f, 0.0f, 0.0f};
        const Vec3 c{0.0f, 1.0f, 0.0f}; // anticlockwise seen from +z: the front faces +z

        assert(hitsFrontFace(a, b, c, Vec3{0.0f, 0.0f, -1.0f}, 1.0f));
        assert(!hitsFrontFace(a, b, c, Vec3{0.1f, 0.2f, 1.0f}, 1.0f));
        assert(!hitsFrontFace(a, b, c, Vec3{0.0f, 0.0f, -1.0f}, -1.0f));
        assert(hitsFrontFace(a, b, c, Vec3{0.0f, 0.0f, 1.0f}, -1.0f));
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected, float tolerance = 1e-5f) {
        return (actual - expected).length() <= tolerance;
    }

    /*
    * The debug view's rays: through the middle of the screen straight along the camera's forward, and through the top
    * right corner along the edge of its view, which the camera's own projection puts at x = y = 1 (so the two agree).
    */
    void testDebugRaysFollowTheCamera() {
        const Camera camera{Vec3{2.0f, 1.0f, 3.0f}, Vec3{-4.0f, 0.5f, -9.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.1f, 16.0f / 9.0f, 0.1f, 500.0f};
        const GpuRayDebugView view = packRayDebugView(camera, RayDebugView::Faces, 1920, 1080);
        const Vec3 right{view.right[0], view.right[1], view.right[2]};
        const Vec3 up{view.up[0], view.up[1], view.up[2]};
        const Vec3 forward{view.forward[0], view.forward[1], view.forward[2]};

        assert(nearlyEqual(cameraRayDirection(right, up, forward, 0.0f, 0.0f), camera.getForward()));
        assert(view.originAndView[0] == 2.0f && view.originAndView[1] == 1.0f && view.originAndView[2] == 3.0f);
        assert(view.originAndView[3] == static_cast<float>(RAY_DEBUG_FACES));
        assert(view.screen[0] == 1920.0f && view.screen[1] == 1080.0f);

        const Vec3 corner = cameraRayDirection(right, up, forward, 1.0f, 1.0f);
        const Mat4 viewProjection = camera.getProjectionMatrix() * camera.getViewMatrix();
        const Vec3 point = camera.getPosition() + corner * 10.0f;
        const Vec4 clip = viewProjection * Vec4{point.x, point.y, point.z, 1.0f};

        assert(std::abs(clip.x / clip.w - 1.0f) < 1e-4f && std::abs(clip.y / clip.w - 1.0f) < 1e-4f);
    }

    // A hit's barycentric coordinates are the second and third corners' weights; the first gets the rest.
    void testCornerWeightsAddUpToOne() {
        assert(nearlyEqual(hitCornerWeights(0.0f, 0.0f), Vec3{1.0f, 0.0f, 0.0f}));
        assert(nearlyEqual(hitCornerWeights(0.25f, 0.5f), Vec3{0.25f, 0.25f, 0.5f}));
    }
}

void testRayTracing() {
    testMasksFollowShadowCasting();
    testInstancesArePackedForTheGpu();
    testFrontFacesAreTheWindingsSide();
    testDebugRaysFollowTheCamera();
    testCornerWeightsAddUpToOne();
}
