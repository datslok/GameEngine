#include "math/frustum.h"
#include "math/mat4.h"
#include "render/gpu/gpu_depth_range.h"
#include "scene/culling.h"
#include "scene/frame_description.h"
#include "scene/model.h"

#include <array>
#include <cassert>
#include <cmath>
#include <numbers>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.0001f;
    }

    bool contains(const std::vector<std::size_t>& indices, std::size_t index) {
        for (std::size_t value : indices) {
            if (value == index) {
                return true;
            }
        }
        return false;
    }

    // A camera at the origin looking down -z, 90 degrees wide, seeing from 1 to 10 units away.
    Mat4 testProjection() {
        return Mat4::perspective(std::numbers::pi_v<float> / 2.0f, 1.0f, 1.0f, 10.0f);
    }

    void checkTestFrustum(const Frustum& frustum) {
        assert(frustum.intersectsSphere(Vec3{0.0f, 0.0f, -5.0f}, 0.0f));

        // Behind the camera, nearer than the near plane, beyond the far plane, and off to the side.
        assert(!frustum.intersectsSphere(Vec3{0.0f, 0.0f, 5.0f}, 0.0f));
        assert(!frustum.intersectsSphere(Vec3{0.0f, 0.0f, -0.5f}, 0.0f));
        assert(!frustum.intersectsSphere(Vec3{0.0f, 0.0f, -11.0f}, 0.0f));
        assert(!frustum.intersectsSphere(Vec3{6.0f, 0.0f, -5.0f}, 0.0f));

        // A sphere whose centre is outside but which pokes in still counts: it is 0.71 from the right side and 2 wide.
        assert(frustum.intersectsSphere(Vec3{6.0f, 0.0f, -5.0f}, 2.0f));
        assert(!frustum.intersectsSphere(Vec3{6.0f, 0.0f, -5.0f}, 0.5f));
    }

    // The six sides come straight out of a view-projection matrix, in either depth convention.
    void testFrustumFromMatrix() {
        checkTestFrustum(Frustum::fromClipMatrix(testProjection(), ClipDepth::NegativeOneToOne));
        checkTestFrustum(Frustum::fromClipMatrix(toGpuDepthRange(testProjection()), ClipDepth::ZeroToOne));

        // A moved camera: the same view shifted 100 units along x.
        const Mat4 moved = testProjection() * Mat4::translation(-100.0f, 0.0f, 0.0f);
        const Frustum frustum = Frustum::fromClipMatrix(moved, ClipDepth::NegativeOneToOne);
        assert(frustum.intersectsSphere(Vec3{100.0f, 0.0f, -5.0f}, 0.0f));
        assert(!frustum.intersectsSphere(Vec3{0.0f, 0.0f, -5.0f}, 0.0f));
    }

    bool hasCorner(const std::array<Vec3, 8>& corners, const Vec3& expected) {
        for (const Vec3& corner : corners) {
            if (std::abs(corner.x - expected.x) < 0.001f && std::abs(corner.y - expected.y) < 0.001f &&
                std::abs(corner.z - expected.z) < 0.001f) {
                return true;
            }
        }
        return false;
    }

    // The eight corners are where three sides meet: the near square 1 unit away and the far square 10 units away.
    void testFrustumCorners() {
        const std::array<Vec3, 8> corners = Frustum::fromClipMatrix(testProjection(), ClipDepth::NegativeOneToOne).corners();

        for (float x : {-1.0f, 1.0f}) {
            for (float y : {-1.0f, 1.0f}) {
                assert(hasCorner(corners, Vec3{x, y, -1.0f}));
                assert(hasCorner(corners, Vec3{x * 10.0f, y * 10.0f, -10.0f}));
            }
        }
    }

    // Two views can only overlap if neither lies wholly outside one side of the other.
    void testFrustumsMayIntersect() {
        const Frustum camera = Frustum::fromClipMatrix(testProjection(), ClipDepth::NegativeOneToOne);
        assert(camera.mayIntersect(camera));

        // The same view moved far to the side.
        const Frustum aside = Frustum::fromClipMatrix(testProjection() * Mat4::translation(-100.0f, 0.0f, 0.0f), ClipDepth::NegativeOneToOne);
        assert(!camera.mayIntersect(aside) && !aside.mayIntersect(camera));

        // A view behind the camera looking further back.
        const Mat4 lookingBack = Mat4::lookAt(Vec3{0.0f, 0.0f, 2.0f}, Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 1.0f, 0.0f});
        const Frustum behind = Frustum::fromClipMatrix(testProjection() * lookingBack, ClipDepth::NegativeOneToOne);
        assert(!camera.mayIntersect(behind) && !behind.mayIntersect(camera));

        // A view from in front looking back at the camera crosses its view.
        const Mat4 facing = Mat4::lookAt(Vec3{0.0f, 0.0f, -12.0f}, Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f});
        const Frustum opposite = Frustum::fromClipMatrix(testProjection() * facing, ClipDepth::NegativeOneToOne);
        assert(camera.mayIntersect(opposite) && opposite.mayIntersect(camera));

        // One wholly inside the other.
        const Frustum narrow = Frustum::fromClipMatrix(Mat4::perspective(0.2f, 1.0f, 2.0f, 5.0f), ClipDepth::NegativeOneToOne);
        assert(camera.mayIntersect(narrow) && narrow.mayIntersect(camera));
    }

    // A box's bounding sphere: its centre moved by the model matrix, its half-diagonal grown by the largest scale.
    void testBoundingSphere() {
        const ModelBounds cube{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};
        const BoundingSphere sphere = boundingSphere(cube, Mat4::translation(5.0f, 0.0f, 0.0f) * Mat4::scaling(2.0f, 1.0f, 1.0f));

        assert(nearlyEqual(sphere.centre.x, 5.0f) && nearlyEqual(sphere.centre.y, 0.0f));
        assert(nearlyEqual(sphere.radius, std::sqrt(3.0f) * 2.0f));

        // An off-centre box keeps its own centre.
        const ModelBounds raised{Vec3{0.0f, 2.0f, 0.0f}, Vec3{2.0f, 4.0f, 2.0f}};
        assert(nearlyEqual(boundingSphere(raised, Mat4::identity()).centre.y, 3.0f));
    }

    DrawItem drawAt(float x, float y, float z, std::uint32_t mesh) {
        DrawItem draw;
        draw.mesh = MeshHandle{mesh};
        draw.model = Mat4::translation(x, y, z);
        return draw;
    }

    // Draws get their spheres from their mesh's bounds; a mesh with unknown bounds gets none and is never culled.
    void testDrawsGetSpheres() {
        std::vector<DrawItem> draws = {drawAt(0.0f, 0.0f, -5.0f, 0), drawAt(0.0f, 0.0f, 5.0f, 9)};
        const std::vector<ModelBounds> bounds = {ModelBounds{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}}};

        attachBoundingSpheres(draws, bounds);

        assert(draws[0].bounds.radius > 0.0f && nearlyEqual(draws[0].bounds.centre.z, -5.0f));
        assert(draws[1].bounds.radius < 0.0f);
        assert(DrawItem{}.bounds.radius < 0.0f);
    }

    // The camera's view skips what it cannot see; draws without known bounds are always kept.
    void testCameraSkipsWhatItCannotSee() {
        std::vector<DrawItem> draws = {drawAt(0.0f, 0.0f, -5.0f, 0), drawAt(0.0f, 0.0f, 5.0f, 0), drawAt(0.0f, 0.0f, 5.0f, 9)};
        attachBoundingSpheres(draws, {ModelBounds{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}}});

        const std::vector<std::size_t> visible = visibleDraws(draws, Frustum::fromClipMatrix(testProjection(), ClipDepth::NegativeOneToOne));

        assert(visible.size() == 2);
        assert(contains(visible, 0) && contains(visible, 2));
    }

    // The classic bug this guards against: something behind the camera can still throw a shadow into view, so shadow
    // passes pick casters with the light's own view, never the camera's.
    void testOffScreenObjectsStillCastShadows() {
        std::vector<DrawItem> draws = {drawAt(0.0f, 0.0f, 5.0f, 0)};
        attachBoundingSpheres(draws, {ModelBounds{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}}});

        const Frustum camera = Frustum::fromClipMatrix(testProjection(), ClipDepth::NegativeOneToOne);
        assert(visibleDraws(draws, camera).empty());

        // A light above, looking down at the object behind the camera.
        const Mat4 lightView = Mat4::lookAt(Vec3{0.0f, 8.0f, 5.0f}, Vec3{0.0f, 0.0f, 5.0f}, Vec3{0.0f, 0.0f, -1.0f});
        const Frustum light = Frustum::fromClipMatrix(testProjection() * lightView, ClipDepth::NegativeOneToOne);
        assert(shadowCasters(draws, light).size() == 1);

        // Things that do not cast shadows are never shadow casters, wherever they are.
        draws[0].castsShadows = false;
        assert(shadowCasters(draws, light).empty());
    }
}

void testCulling() {
    testFrustumFromMatrix();
    testFrustumCorners();
    testFrustumsMayIntersect();
    testBoundingSphere();
    testDrawsGetSpheres();
    testCameraSkipsWhatItCannotSee();
    testOffScreenObjectsStillCastShadows();
}
