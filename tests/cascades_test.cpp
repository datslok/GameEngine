#include "render/gpu/cascades.h"
#include "render/gpu/soft_shadow.h"
#include "scene/camera.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <optional>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected, float tolerance = 0.0001f) {
        return std::abs(actual - expected) < tolerance;
    }

    Vec3 project(const Mat4& tileMatrix, const Vec3& point) {
        const Vec4 clip = tileMatrix * Vec4{point.x, point.y, point.z, 1.0f};
        return Vec3{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
    }

    Camera cameraAt(const Vec3& position, const Vec3& target, float fov = 1.2f, float aspect = 16.0f / 9.0f) {
        return Camera{position, target, Vec3{0.0f, 1.0f, 0.0f}, fov, aspect, 0.1f, 1000.0f};
    }

    // The eight corners of the camera's view between two distances along it.
    std::vector<Vec3> sliceCorners(const Camera& camera, float nearDistance, float farDistance) {
        const float halfHeight = std::tan(camera.getVerticalFov() * 0.5f);
        const float halfWidth = halfHeight * camera.getAspectRatio();
        std::vector<Vec3> corners;
        // getUp is the world's up; the view's own up axis is square to its forward and right.
        const Vec3 viewUp = camera.getRight().cross(camera.getForward());

        for (float distance : {nearDistance, farDistance}) {
            for (float x : {-1.0f, 1.0f}) {
                for (float y : {-1.0f, 1.0f}) {
                    corners.push_back(camera.getPosition() + camera.getForward() * distance +
                                      camera.getRight() * (x * halfWidth * distance) + viewUp * (y * halfHeight * distance));
                }
            }
        }

        return corners;
    }

    // The splits start at the near distance, end at the far one, and only grow.
    void testSplitsRunFromNearToFar() {
        const std::vector<float> splits = cascadeSplits(0.1f, 200.0f, 4, 0.75f);

        assert(splits.size() == 5);
        assert(nearlyEqual(splits.front(), 0.1f));
        assert(nearlyEqual(splits.back(), 200.0f, 0.001f));

        for (std::size_t i = 1; i < splits.size(); ++i) {
            assert(splits[i] > splits[i - 1]);
        }

        assert(cascadeSplits(0.1f, 50.0f, 1, 0.75f).size() == 2);
    }

    /*
    * Lambda 0 spaces the splits evenly; 1 spaces them logarithmically, each cascade reaching the same multiple further
    * than the last (here 10 times: 1, 10, 100, 1000); between, a straight blend of the two.
    */
    void testLambdaBlendsEvenAndLogarithmicSplits() {
        const std::vector<float> even = cascadeSplits(1.0f, 1000.0f, 3, 0.0f);
        assert(nearlyEqual(even[1], 334.0f, 0.01f) && nearlyEqual(even[2], 667.0f, 0.01f));

        const std::vector<float> logarithmic = cascadeSplits(1.0f, 1000.0f, 3, 1.0f);
        assert(nearlyEqual(logarithmic[1], 10.0f, 0.001f) && nearlyEqual(logarithmic[2], 100.0f, 0.01f));

        const std::vector<float> half = cascadeSplits(1.0f, 1000.0f, 3, 0.5f);
        assert(nearlyEqual(half[1], 0.5f * (334.0f + 10.0f), 0.01f));
    }

    // Every corner of the slice is inside its sphere, and the sphere is no bigger than it must be: some corner touches it.
    void testSphereHoldsItsSlice() {
        const Camera cameras[] = {
            cameraAt(Vec3{0.0f, 2.0f, 0.0f}, Vec3{0.0f, 2.0f, -10.0f}),
            cameraAt(Vec3{30.0f, 5.0f, -12.0f}, Vec3{10.0f, 0.0f, 40.0f}, 0.6f, 1.0f),
            cameraAt(Vec3{-4.0f, 80.0f, 9.0f}, Vec3{0.0f, 0.0f, 0.0f}, 1.6f, 2.4f)
        };
        const float slices[][2] = {{0.1f, 5.0f}, {5.0f, 30.0f}, {30.0f, 200.0f}, {0.1f, 200.0f}};

        for (const Camera& camera : cameras) {
            for (const auto& slice : slices) {
                const BoundingSphere sphere = cascadeSphere(camera, slice[0], slice[1]);
                float farthest = 0.0f;

                for (const Vec3& corner : sliceCorners(camera, slice[0], slice[1])) {
                    const float distance = (corner - sphere.centre).length();
                    assert(distance <= sphere.radius * 1.0001f);
                    farthest = std::max(farthest, distance);
                }

                assert(farthest >= sphere.radius * 0.999f);
            }
        }
    }

    // The sphere's size comes from the lens alone: wherever the camera is and wherever it points, the same slice gets
    // exactly the same radius, so the shadow box never changes size as the camera turns.
    void testSphereSizeDoesNotTurnWithTheCamera() {
        const float radius = cascadeSphere(cameraAt(Vec3{0.0f, 2.0f, 0.0f}, Vec3{0.0f, 2.0f, -10.0f}), 5.0f, 30.0f).radius;

        assert(cascadeSphere(cameraAt(Vec3{40.0f, 9.0f, 3.0f}, Vec3{-7.0f, 0.0f, 11.0f}), 5.0f, 30.0f).radius == radius);
        assert(cascadeSphere(cameraAt(Vec3{-1.0f, 50.0f, 2.0f}, Vec3{0.0f, 0.0f, 0.0f}), 5.0f, 30.0f).radius == radius);

        // A wider view needs a bigger sphere.
        assert(cascadeSphere(cameraAt(Vec3{0.0f, 2.0f, 0.0f}, Vec3{0.0f, 2.0f, -10.0f}, 1.6f), 5.0f, 30.0f).radius > radius);
    }

    // The whole sphere lands inside the box, and so does a point between it and the light, up to the caster reach.
    void testBoxHoldsItsSphere() {
        const Vec3 sunlight = Vec3{0.4f, -1.0f, -0.6f}.normalized();
        const BoundingSphere sphere{Vec3{3.0f, 1.0f, -20.0f}, 12.0f};
        const CascadeBox box = cascadeBox(sunlight, sphere, 50.0f, 1024);

        const Vec3 directions[] = {
            Vec3{1.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
            Vec3{0.0f, -1.0f, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, Vec3{0.0f, 0.0f, -1.0f},
            Vec3{1.0f, 1.0f, 1.0f}.normalized(), Vec3{-1.0f, 0.5f, 0.2f}.normalized(), sunlight, sunlight * -1.0f
        };

        for (const Vec3& direction : directions) {
            const Vec3 onSurface = project(box.matrix, sphere.centre + direction * (sphere.radius * 0.999f));
            assert(std::abs(onSurface.x) <= 1.0f && std::abs(onSurface.y) <= 1.0f);
            assert(onSurface.z > 0.0f && onSurface.z < 1.0f);
        }

        const Vec3 towardsLight = project(box.matrix, sphere.centre - sunlight * (sphere.radius + 45.0f));
        assert(towardsLight.z > 0.0f && towardsLight.z < 1.0f);

        // Further from the sun means a smaller depth (reversed-Z).
        assert(project(box.matrix, sphere.centre).z > project(box.matrix, sphere.centre + sunlight * 5.0f).z);
    }

    // The box is the sphere's width across (2 r, in tileSize texels) and its depth plus the reach along the light.
    void testBoxKnowsItsTexelAndDepth() {
        const Vec3 sunlight = Vec3{0.0f, -1.0f, 0.0f};
        const BoundingSphere sphere{Vec3{0.0f, 0.0f, 0.0f}, 8.0f};
        const CascadeBox box = cascadeBox(sunlight, sphere, 30.0f, 1024);

        assert(nearlyEqual(box.texelSize, 16.0f / 1024.0f));
        assert(nearlyEqual(box.depthRange, 46.0f));

        // The sphere's centre is the reach plus a radius from the box's near side.
        assert(nearlyEqual(boxShadowDistance(project(box.matrix, sphere.centre).z, box.depthRange), 38.0f, 0.01f));
    }

    // As the sphere moves, the box follows in whole texels, so what it draws lands on the same texels.
    void testBoxMovesInWholeTexels() {
        const Vec3 sunlight{0.4f, -1.0f, -0.6f};
        const Vec3 fixedPoint{1.0f, 0.0f, -3.0f};
        const std::uint32_t tileSize = 1024;
        const float texelsPerUnit = static_cast<float>(tileSize) * 0.5f;
        const Vec3 before = project(cascadeBox(sunlight, BoundingSphere{Vec3{2.0f, 5.0f, 4.0f}, 10.0f}, 50.0f, tileSize).matrix, fixedPoint);

        for (int step = 1; step < 6; ++step) {
            const float nudge = 0.013f * static_cast<float>(step);
            const BoundingSphere moved{Vec3{2.0f + nudge, 5.0f, 4.0f - nudge}, 10.0f};
            const Vec3 after = project(cascadeBox(sunlight, moved, 50.0f, tileSize).matrix, fixedPoint);

            const float shiftX = (after.x - before.x) * texelsPerUnit;
            const float shiftY = (after.y - before.y) * texelsPerUnit;

            assert(nearlyEqual(shiftX, std::round(shiftX), 0.01f));
            assert(nearlyEqual(shiftY, std::round(shiftY), 0.01f));
        }
    }

    /*
    * 1 at the centre, 0 on any edge of the box, negative outside, in units of the radius. Across the light that is
    * 1 - |x| (or y); along it, the distance from the far side (depth 0) or from the near side (depth 1), scaled to radii.
    */
    void testEdgeDistance() {
        // A box 3 radii deep (a radius of reach behind the sphere): its centre is 1 radius from the far side, at depth 1/3.
        const float depthOverRadius = 3.0f;

        assert(nearlyEqual(cascadeEdgeDistance(Vec3{0.0f, 0.0f, 1.0f / 3.0f}, depthOverRadius), 1.0f));
        assert(nearlyEqual(cascadeEdgeDistance(Vec3{0.75f, 0.0f, 1.0f / 3.0f}, depthOverRadius), 0.25f));
        assert(nearlyEqual(cascadeEdgeDistance(Vec3{0.0f, -0.9f, 1.0f / 3.0f}, depthOverRadius), 0.1f));
        assert(nearlyEqual(cascadeEdgeDistance(Vec3{0.0f, 0.0f, 0.1f}, depthOverRadius), 0.3f));
        assert(nearlyEqual(cascadeEdgeDistance(Vec3{0.0f, 0.0f, 0.95f}, depthOverRadius), 0.15f));
        assert(nearlyEqual(cascadeEdgeDistance(Vec3{1.0f, 0.0f, 0.5f}, depthOverRadius), 0.0f));

        assert(cascadeEdgeDistance(Vec3{1.2f, 0.0f, 0.5f}, depthOverRadius) < 0.0f);
        assert(cascadeEdgeDistance(Vec3{0.0f, 0.0f, -0.1f}, depthOverRadius) < 0.0f);
        assert(cascadeEdgeDistance(Vec3{0.0f, 0.0f, 1.1f}, depthOverRadius) < 0.0f);
    }

    // Deeper than the band inside, a cascade gives all of the shadow; at its edge none; in between a straight ramp.
    void testBlendWeight() {
        assert(cascadeBlendWeight(0.5f, 0.1f) == 1.0f);
        assert(cascadeBlendWeight(0.1f, 0.1f) == 1.0f);
        assert(nearlyEqual(cascadeBlendWeight(0.05f, 0.1f), 0.5f));
        assert(cascadeBlendWeight(0.0f, 0.1f) == 0.0f);
        assert(cascadeBlendWeight(-0.2f, 0.1f) == 0.0f);

        // No band: straight from one cascade to the next.
        assert(cascadeBlendWeight(0.001f, 0.0f) == 1.0f);
        assert(cascadeBlendWeight(-0.001f, 0.0f) == 0.0f);
    }

    // The dithered switch keeps this cascade for a share of pixels equal to its weight, spread by their noise.
    void testDitherKeepsTheWeightsShare() {
        int kept = 0;

        for (int i = 0; i < 1000; ++i) {
            const float noise = (static_cast<float>(i) + 0.5f) / 1000.0f;
            assert(cascadeDitherKeeps(1.0f, noise));
            assert(!cascadeDitherKeeps(0.0f, noise));
            kept += cascadeDitherKeeps(0.3f, noise) ? 1 : 0;
        }

        assert(kept == 300);
    }
    /*
    * Moving a point straight towards the sun leaves it where it was in every cascade's map (only its depth changes), so
    * the bias against acne cannot move a shadow's edge, and two cascades with different texels agree on where it is.
    */
    void testMovingTowardsTheSunKeepsThePlaceInEveryCascade() {
        const Vec3 sunlight = Vec3{0.4f, -0.3f, -0.6f}.normalized();
        const Vec3 point{2.0f, 0.0f, -7.0f};
        const Vec3 moved = point - sunlight * 0.8f;

        for (float radius : {4.5f, 13.0f, 290.0f}) {
            const CascadeBox box = cascadeBox(sunlight, BoundingSphere{Vec3{1.0f, 0.5f, -5.0f}, radius}, 200.0f, 1024);
            const Vec3 before = project(box.matrix, point);
            const Vec3 after = project(box.matrix, moved);

            assert(nearlyEqual(before.x, after.x, 0.00001f) && nearlyEqual(before.y, after.y, 0.00001f));
            assert(after.z > before.z);
        }
    }

    /*
    * The bias is the cascade's base offset where the sun shines straight down on a surface, and grows with the slope
    * (the tangent of the angle between the normal and the sun) where it grazes, since a texel then spans more depth;
    * capped, so surfaces almost edge-on to the sun do not lift their lookups far off.
    */
    void testSunBiasGrowsWithTheSlope() {
        assert(nearlyEqual(sunShadowBias(0.01f, 1.0f), 0.01f));
        assert(nearlyEqual(sunShadowBias(0.01f, std::sqrt(0.5f)), 0.02f));
        assert(nearlyEqual(sunShadowBias(0.01f, 0.0f), 0.01f * (1.0f + maxSunBiasSlope)));
        assert(nearlyEqual(sunShadowBias(0.01f, -0.5f), 0.01f * (1.0f + maxSunBiasSlope)));
        assert(sunShadowBias(0.01f, 0.3f) > sunShadowBias(0.01f, 0.6f));
    }

    ShadowDistanceFit fitFrom(float farthestSurface) {
        return fitShadowDistance(ShadowDistanceFit{}, farthestSurface);
    }

    // Before anything was measured, or with no surface on screen, nothing is fitted: the settings' distance applies.
    void testNothingMeasuredIsNotFitted() {
        assert(!ShadowDistanceFit{}.step.has_value());
        assert(std::isinf(fittedShadowDistance(ShadowDistanceFit{})));
        assert(!fitShadowDistance(fitFrom(7.0f), std::nullopt).step.has_value());
    }

    /*
    * The fitted distance is the farthest surface plus the margin, rounded up to the next step of the ladder (each step
    * shadowDistanceStep times the last, from 1 m): never short of the margin, and never a whole step past it.
    */
    void testTheFitRoundsUpToTheLadder() {
        for (float farthest : {0.3f, 1.0f, 2.2f, 7.0f, 7.45f, 38.0f, 150.0f, 199.0f}) {
            const float distance = fittedShadowDistance(fitFrom(farthest));
            const float needed = std::max(farthest * shadowDistanceMargin, 1.0f);

            assert(distance >= needed);
            assert(distance < needed * shadowDistanceStep * 1.0001f);

            const float steps = std::log(distance) / std::log(shadowDistanceStep);
            assert(nearlyEqual(steps, std::round(steps), 0.001f));
        }

        // A surface nearer than a metre still gets the first step.
        assert(fitFrom(0.01f).step == 0);
        assert(nearlyEqual(fittedShadowDistance(fitFrom(0.01f)), 1.0f));
    }

    // Small changes in the farthest surface stay on one step, so the cascades keep their size and stay cached.
    void testSmallChangesKeepTheStep() {
        const ShadowDistanceFit fit = fitFrom(7.0f);
        assert(fitShadowDistance(fit, 7.3f).step == fit.step);
        assert(fitShadowDistance(fit, 6.6f).step == fit.step);
    }

    /*
    * More to see moves out at once, so nothing on screen is left without a shadow; less moves in only once the need is
    * shadowDistanceShrinkSteps steps lower, so a view hovering at a step's edge does not flip between two sizes.
    */
    void testTheFitGrowsAtOnceAndShrinksLate() {
        const ShadowDistanceFit fit = fitFrom(7.0f);
        const int step = *fit.step;

        const ShadowDistanceFit grown = fitShadowDistance(fit, 20.0f);
        assert(*grown.step > step + 1);
        assert(fittedShadowDistance(grown) >= 20.0f * shadowDistanceMargin);

        // One step lower is not enough to shrink; two are.
        const float oneLower = std::pow(shadowDistanceStep, static_cast<float>(step - 1)) / shadowDistanceMargin * 0.999f;
        assert(*fitFrom(oneLower).step == step - 1);
        assert(fitShadowDistance(fit, oneLower).step == fit.step);

        const float twoLower = std::pow(shadowDistanceStep, static_cast<float>(step - 2)) / shadowDistanceMargin * 0.999f;
        assert(*fitShadowDistance(fit, twoLower).step == step - 2);
    }
}

void testCascades() {
    testNothingMeasuredIsNotFitted();
    testTheFitRoundsUpToTheLadder();
    testSmallChangesKeepTheStep();
    testTheFitGrowsAtOnceAndShrinksLate();
    testMovingTowardsTheSunKeepsThePlaceInEveryCascade();
    testSunBiasGrowsWithTheSlope();
    testSplitsRunFromNearToFar();
    testLambdaBlendsEvenAndLogarithmicSplits();
    testSphereHoldsItsSlice();
    testSphereSizeDoesNotTurnWithTheCamera();
    testBoxHoldsItsSphere();
    testBoxKnowsItsTexelAndDepth();
    testBoxMovesInWholeTexels();
    testEdgeDistance();
    testBlendWeight();
    testDitherKeepsTheWeightsShare();
}
