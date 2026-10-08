#include "render/gpu/light_uniforms.h"
#include "scene/lighting.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }

    bool nearlyEqual(const float (&actual)[4], float x, float y, float z, float w) {
        return nearlyEqual(actual[0], x) && nearlyEqual(actual[1], y) && nearlyEqual(actual[2], z) && nearlyEqual(actual[3], w);
    }

    PlacedPointLight pointLightAt(Vec3 position, float range) {
        return PlacedPointLight{position, PointLight{Vec3{1.0f, 1.0f, 1.0f}, 1.0f, range}};
    }

    void testAmbientAndEmptyCounts() {
        FrameLighting lighting;
        lighting.ambient = Vec3{0.2f, 0.2f, 0.2f};

        const LightUniformData data = packLighting(lighting);

        assert(nearlyEqual(data.ambient, 0.2f, 0.2f, 0.2f, 0.0f));
        assert(data.counts[0] == 0 && data.counts[1] == 0 && data.counts[2] == 0 && data.counts[3] == 0);
    }

    // The shader wants the direction towards the light, normalised, and colour already multiplied by intensity.
    void testDirectionalLightIsFlippedAndPremultiplied() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.0f, -2.0f, 0.0f}, Vec3{1.0f, 0.5f, 0.5f}, 2.0f});

        const LightUniformData data = packLighting(lighting);

        assert(data.counts[0] == 1);
        assert(nearlyEqual(data.directional[0].toLight, 0.0f, 1.0f, 0.0f, 0.0f));
        assert(nearlyEqual(data.directional[0].radiance, 2.0f, 1.0f, 1.0f, 0.0f));
    }

    // A zero direction cannot be normalised, so it must not take a slot.
    void testZeroDirectionIsSkipped() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f});
        lighting.directionalLights.push_back(DirectionalLight{Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f});

        const LightUniformData data = packLighting(lighting);

        assert(data.counts[0] == 1);
        assert(nearlyEqual(data.directional[0].toLight, -1.0f, 0.0f, 0.0f, 0.0f));
    }

    void testPointLightIsPacked() {
        FrameLighting lighting;
        lighting.pointLights.push_back(PlacedPointLight{Vec3{1.0f, 2.0f, 3.0f}, PointLight{Vec3{1.0f, 1.0f, 1.0f}, 3.0f, 6.0f}});

        const LightUniformData data = packLighting(lighting);

        assert(data.counts[1] == 1);
        assert(nearlyEqual(data.points[0].positionRange, 1.0f, 2.0f, 3.0f, 6.0f));
        assert(nearlyEqual(data.points[0].radiance, 3.0f, 3.0f, 3.0f, 0.0f));
    }

    // The shader divides by range, so a light without a positive range would turn pixels into NaN.
    void testPointLightWithoutRangeIsSkipped() {
        FrameLighting lighting;
        lighting.pointLights.push_back(pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, 0.0f));
        lighting.pointLights.push_back(pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, -1.0f));

        const LightUniformData data = packLighting(lighting);

        assert(data.counts[1] == 0);
    }

    // Lights past the limits are dropped; the first ones collected win.
    void testCountsAreCapped() {
        FrameLighting lighting;

        for (int i = 0; i < 6; ++i) {
            lighting.directionalLights.push_back(DirectionalLight{});
        }

        for (int i = 0; i < 20; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, 5.0f));
        }

        const LightUniformData data = packLighting(lighting);

        assert(data.counts[0] == maxDirectionalLights);
        assert(data.counts[1] == maxPointLights);
        assert(nearlyEqual(data.points[maxPointLights - 1].positionRange, 15.0f, 0.0f, 0.0f, 5.0f));
    }
}

void testLightUniforms() {
    testAmbientAndEmptyCounts();
    testDirectionalLightIsFlippedAndPremultiplied();
    testZeroDirectionIsSkipped();
    testPointLightIsPacked();
    testPointLightWithoutRangeIsSkipped();
    testCountsAreCapped();
}
