#include "math/mat4.h"
#include "render/gpu/camera_relative.h"
#include "render/gpu/gpu_depth_range.h"
#include "scene/camera.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance) {
        return std::abs(actual - expected) < tolerance;
    }

    // The world-space way, as the renderer drew before: the full view (with the camera's position in it) times the model.
    Vec4 worldPathClip(const Camera& camera, const Mat4& model, const Vec3& vertex) {
        const Mat4 transform = toGpuDepthRange(camera.getProjectionMatrix()) * camera.getViewMatrix() * model;
        return transform * Vec4{vertex.x, vertex.y, vertex.z, 1.0f};
    }

    Vec4 relativePathClip(const Camera& camera, const Mat4& model, const Vec3& vertex) {
        const Mat4 transform = cameraRelativeViewProjection(camera) * relativeTo(model, camera.getPosition());
        return transform * Vec4{vertex.x, vertex.y, vertex.z, 1.0f};
    }

    // Moving a model matrix to a new origin changes its translation by exactly the origin and nothing else.
    void testRelativeToMovesOnlyTheTranslation() {
        const Mat4 model = Mat4::translation(5.0f, 6.0f, 7.0f) * Mat4::rotationY(0.3f) * Mat4::scaling(2.0f, 1.0f, 3.0f);
        const Mat4 moved = relativeTo(model, Vec3{1.0f, 2.0f, 3.0f});

        assert(moved.values[0][3] == 4.0f && moved.values[1][3] == 4.0f && moved.values[2][3] == 4.0f);

        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 3; ++column) {
                assert(moved.values[row][column] == model.values[row][column]);
            }
        }

        assert(moved.values[3][3] == 1.0f);
    }

    // Near the world's origin both ways agree: drawing relative to the camera changes how the sum is done, not the picture.
    void testBothPathsAgreeNearTheOrigin() {
        const Camera camera{Vec3{2.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.0f, -5.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.2f, 1.5f, 0.1f, 100.0f};
        const Mat4 model = Mat4::translation(-1.0f, 0.5f, -6.0f) * Mat4::rotationX(0.4f);

        for (const Vec3& vertex : {Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, -0.5f, 0.25f}, Vec3{-2.0f, 1.0f, 3.0f}}) {
            const Vec4 world = worldPathClip(camera, model, vertex);
            const Vec4 relative = relativePathClip(camera, model, vertex);

            assert(nearlyEqual(relative.x, world.x, 1e-4f) && nearlyEqual(relative.y, world.y, 1e-4f));
            assert(nearlyEqual(relative.z, world.z, 1e-4f) && nearlyEqual(relative.w, world.w, 1e-4f));
        }
    }

    /*
    * Twelve kilometres out, a float's steps are about 1 mm apart. The world-space way puts the camera's position into
    * the view matrix and the object's into the model matrix, and multiplying them subtracts two numbers near 12345 that
    * are each rounded to that millimetre: the error lands on screen as vertices jittering as the camera moves. Moving
    * the object to the camera first is one subtraction whose result (a few metres) is exact to within its own tiny
    * rounding. The screen positions are compared with the exact answer, worked out in doubles.
    */
    void testRelativePathStaysSharpFarFromTheOrigin() {
        const Vec3 cameraPosition{12345.67f, 1.7f, -4321.5f};
        const Camera camera{cameraPosition, cameraPosition + Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.2f, 1.5f, 0.1f, 100.0f};
        const Mat4 model = Mat4::translation(cameraPosition.x + 0.37f, cameraPosition.y - 0.2f, cameraPosition.z - 3.0f);
        const Mat4 projection = camera.getProjectionMatrix();

        float worldError = 0.0f;
        float relativeError = 0.0f;

        for (const Vec3& vertex : {Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.01f, 0.02f, 0.0f}, Vec3{-0.013f, 0.007f, 0.1f},
                                   Vec3{0.25f, -0.125f, -0.5f}, Vec3{-0.031f, -0.017f, 0.3f}}) {
            // The camera looks straight down -z, so the exact screen position is the projection's scale times the
            // offset from the camera, divided by the distance in front of it.
            const double offsetX = (static_cast<double>(model.values[0][3]) + vertex.x) - cameraPosition.x;
            const double offsetY = (static_cast<double>(model.values[1][3]) + vertex.y) - cameraPosition.y;
            const double ahead = cameraPosition.z - (static_cast<double>(model.values[2][3]) + vertex.z);
            const double exactX = projection.values[0][0] * offsetX / ahead;
            const double exactY = projection.values[1][1] * offsetY / ahead;

            const Vec4 world = worldPathClip(camera, model, vertex);
            const Vec4 relative = relativePathClip(camera, model, vertex);

            worldError = std::max({worldError, static_cast<float>(std::abs(world.x / world.w - exactX)),
                                   static_cast<float>(std::abs(world.y / world.w - exactY))});
            relativeError = std::max({relativeError, static_cast<float>(std::abs(relative.x / relative.w - exactX)),
                                      static_cast<float>(std::abs(relative.y / relative.w - exactY))});
        }

        assert(relativeError < 1e-5f);
        assert(worldError > 20.0f * relativeError);
    }
}

void testCameraRelative() {
    testRelativeToMovesOnlyTheTranslation();
    testBothPathsAgreeNearTheOrigin();
    testRelativePathStaysSharpFarFromTheOrigin();
}
