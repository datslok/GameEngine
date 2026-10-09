#include "render/gpu/light_uniforms.h"
#include "render/gpu/point_shadow.h"
#include "scene/lighting.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 0.0001f) {
        return std::abs(actual - expected) < tolerance;
    }

    // Where a world point lands in a face's image: x and y from -1 to 1 across the face, depth from 0 (near) to 1 (far).
    Vec3 project(const Mat4& faceMatrix, const Vec3& point) {
        const Vec4 clip = faceMatrix * Vec4{point.x, point.y, point.z, 1.0f};
        return Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
    }

    // The face is picked by the direction's largest component, like a cube map: +X, -X, +Y, -Y, +Z, -Z.
    void testFaceFollowsTheLargestAxis() {
        assert(pointShadowFace(Vec3{5.0f, 1.0f, -2.0f}) == 0);
        assert(pointShadowFace(Vec3{-5.0f, 1.0f, 2.0f}) == 1);
        assert(pointShadowFace(Vec3{0.1f, 3.0f, 0.0f}) == 2);
        assert(pointShadowFace(Vec3{0.0f, -3.0f, 0.1f}) == 3);
        assert(pointShadowFace(Vec3{0.0f, 0.0f, 2.0f}) == 4);
        assert(pointShadowFace(Vec3{1.0f, 1.0f, -3.0f}) == 5);
    }

    // Each face is a camera at the light looking along its axis: a point straight ahead lands in the middle of the image.
    void testEachFaceLooksAlongItsAxis() {
        const Vec3 light{1.0f, 2.0f, 3.0f};
        const Vec3 axes[6] = {
            Vec3{1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f},
            Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f},
            Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, -1.0f}
        };

        for (int face = 0; face < 6; ++face) {
            const Vec3 ahead = project(pointShadowFaceMatrix(light, face, 50.0f), light + axes[face] * 10.0f);

            assert(nearlyEqual(ahead.x, 0.0f) && nearlyEqual(ahead.y, 0.0f));
            assert(ahead.z > 0.0f && ahead.z < 1.0f);
        }
    }

    // Every direction lands on the face chosen for it, and not right at the edge: the faces are a little wider than
    // 90 degrees, so the soft-shadow samples around a point never fall off its face.
    void testEveryDirectionLandsInsideItsFace() {
        const Vec3 light{0.0f, 1.0f, 0.0f};
        const float steps[5] = {-1.0f, -0.5f, 0.0f, 0.5f, 1.0f};
        const float limit = 1.0f - 3.0f / static_cast<float>(pointShadowFaceSize); // 1.5 texels from the edge

        for (float x : steps) {
            for (float y : steps) {
                for (float z : steps) {
                    const Vec3 direction{x, y, z};

                    if (direction.lengthSquared() == 0.0f) {
                        continue;
                    }

                    const int face = pointShadowFace(direction);
                    const Vec3 landed = project(pointShadowFaceMatrix(light, face, 50.0f), light + direction * 7.0f);

                    assert(std::abs(landed.x) <= limit && std::abs(landed.y) <= limit);
                    assert(landed.z > 0.0f && landed.z < 1.0f);
                }
            }
        }
    }

    // Depth grows with distance from the light, from 0 at the near plane to 1 at the far plane (the light's range).
    void testDepthGrowsWithDistance() {
        const Vec3 light{0.0f, 0.0f, 0.0f};
        const Mat4 face = pointShadowFaceMatrix(light, 0, 50.0f);

        assert(nearlyEqual(project(face, Vec3{pointShadowNearPlane, 0.0f, 0.0f}).z, 0.0f));
        assert(nearlyEqual(project(face, Vec3{50.0f, 0.0f, 0.0f}).z, 1.0f));
        assert(project(face, Vec3{10.0f, 0.0f, 0.0f}).z < project(face, Vec3{20.0f, 0.0f, 0.0f}).z);
    }

    // The shader gets the shadowed light's six face matrices, column-major like every matrix pushed to it.
    void testShadowedLightIsPacked() {
        FrameLighting lighting;
        PlacedPointLight moon{Vec3{0.0f, 40.0f, -50.0f}, PointLight{}};
        moon.light.range = 150.0f;
        moon.light.castsShadows = true;
        lighting.pointLights.push_back(moon);

        const ShadowUniformData data = packPointShadow(packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f}));

        for (int face = 0; face < 6; ++face) {
            const Mat4 expected = pointShadowFaceMatrix(moon.position, face, 150.0f);

            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 4; ++column) {
                    assert(nearlyEqual(data.faceMatrices[face][column * 4 + row], expected.values[row][column]));
                }
            }
        }

        // The normal offset grows with distance, because a shadow texel covers more of the world further from the light.
        assert(data.settings[0] > 0.0f);
    }
}

void testPointShadow() {
    testFaceFollowsTheLargestAxis();
    testEachFaceLooksAlongItsAxis();
    testEveryDirectionLandsInsideItsFace();
    testDepthGrowsWithDistance();
    testShadowedLightIsPacked();
}
