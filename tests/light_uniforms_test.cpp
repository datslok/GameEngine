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

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(nearlyEqual(data.ambient, 0.2f, 0.2f, 0.2f, 0.0f));
        assert(data.counts[0] == 0 && data.counts[1] == 0 && data.counts[2] == 0);

        // No point light casts a shadow.
        assert(data.counts[3] == -1);
    }

    // The shader wants the direction towards the light, normalised, and colour already multiplied by intensity.
    void testDirectionalLightIsFlippedAndPremultiplied() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.0f, -2.0f, 0.0f}, Vec3{1.0f, 0.5f, 0.5f}, 2.0f});

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[0] == 1);
        assert(nearlyEqual(data.directional[0].toLight, 0.0f, 1.0f, 0.0f, 0.0f));
        assert(nearlyEqual(data.directional[0].radiance, 2.0f, 1.0f, 1.0f, 0.0f));
    }

    // A zero direction cannot be normalised, so it must not take a slot.
    void testZeroDirectionIsSkipped() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f});
        lighting.directionalLights.push_back(DirectionalLight{Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f});

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[0] == 1);
        assert(nearlyEqual(data.directional[0].toLight, -1.0f, 0.0f, 0.0f, 0.0f));
    }

    void testPointLightIsPacked() {
        FrameLighting lighting;
        lighting.pointLights.push_back(PlacedPointLight{Vec3{1.0f, 2.0f, 3.0f}, PointLight{Vec3{1.0f, 1.0f, 1.0f}, 3.0f, 6.0f}});

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[1] == 1);
        assert(nearlyEqual(data.points[0].positionRange, 1.0f, 2.0f, 3.0f, 6.0f));
        assert(nearlyEqual(data.points[0].radiance, 3.0f, 3.0f, 3.0f, 1.0f)); // w: source radius, 1 by default
    }

    // The shader divides by range, so a light without a positive range would turn pixels into NaN.
    void testPointLightWithoutRangeIsSkipped() {
        FrameLighting lighting;
        lighting.pointLights.push_back(pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, 0.0f));
        lighting.pointLights.push_back(pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, -1.0f));

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[1] == 0);
    }

    // Specular highlights depend on where the viewer is, so the camera position travels with the lights.
    void testCameraPositionIsPacked() {
        const LightUniformData data = packLighting(FrameLighting{}, Vec3{1.0f, 2.0f, 3.0f});

        assert(nearlyEqual(data.cameraPosition, 1.0f, 2.0f, 3.0f, 0.0f));
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

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[0] == maxDirectionalLights);
        assert(data.counts[1] == maxPointLights);
        assert(nearlyEqual(data.points[maxPointLights - 1].positionRange, 15.0f, 0.0f, 0.0f, 5.0f));
    }
}

namespace {
    PlacedSpotLight spotLightAt(Vec3 position, Vec3 direction, float range, float innerAngle, float outerAngle) {
        return PlacedSpotLight{position, SpotLight{Vec3{1.0f, 0.5f, 0.5f}, 2.0f, range, direction, innerAngle, outerAngle}};
    }

    // The shader compares cosines, so the cone angles arrive as cosines, computed once here instead of per pixel.
    void testSpotLightIsPacked() {
        FrameLighting lighting;
        lighting.spotLights.push_back(spotLightAt(Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.0f, 0.0f, -4.0f}, 20.0f, 0.2f, 0.4f));

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[2] == 1);
        assert(nearlyEqual(data.spots[0].positionRange, 1.0f, 2.0f, 3.0f, 20.0f));
        assert(nearlyEqual(data.spots[0].directionCosOuter, 0.0f, 0.0f, -1.0f, std::cos(0.4f)));
        assert(nearlyEqual(data.spots[0].radianceCosInner, 2.0f, 1.0f, 1.0f, std::cos(0.2f)));
    }

    // A beam needs a direction and a reach, like the other lights.
    void testSpotLightWithoutDirectionOrRangeIsSkipped() {
        FrameLighting lighting;
        lighting.spotLights.push_back(spotLightAt(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, 0.0f}, 20.0f, 0.2f, 0.4f));
        lighting.spotLights.push_back(spotLightAt(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, 0.0f, 0.2f, 0.4f));

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[2] == 0);
    }

    // An inner cone wider than the outer one would make the smooth edge run backwards, so it is clamped to the outer cone.
    void testSpotLightInnerConeIsClamped() {
        FrameLighting lighting;
        lighting.spotLights.push_back(spotLightAt(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f, 0.6f, 0.3f));

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(nearlyEqual(data.spots[0].radianceCosInner[3], std::cos(0.3f)));
        assert(nearlyEqual(data.spots[0].directionCosOuter[3], std::cos(0.3f)));
    }

    void testSpotLightsAreCapped() {
        FrameLighting lighting;

        for (int i = 0; i < 6; ++i) {
            lighting.spotLights.push_back(spotLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f, 0.2f, 0.4f));
        }

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[2] == maxSpotLights);
        assert(nearlyEqual(data.spots[maxSpotLights - 1].positionRange, 3.0f, 0.0f, 0.0f, 20.0f));
    }
}

namespace {
    // A larger source softens the light up close; a radius that is not positive falls back to 1 so the falloff never divides by zero.
    void testSpotLightSourceRadiusIsPacked() {
        FrameLighting lighting;

        PlacedSpotLight wide = spotLightAt(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f, 0.2f, 0.4f);
        wide.light.sourceRadius = 3.0f;
        lighting.spotLights.push_back(wide);

        PlacedSpotLight broken = wide;
        broken.light.sourceRadius = 0.0f;
        lighting.spotLights.push_back(broken);

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(nearlyEqual(data.spots[0].sourceRadius[0], 3.0f));
        assert(nearlyEqual(data.spots[1].sourceRadius[0], 1.0f));
        assert(nearlyEqual(SpotLight{}.sourceRadius, 1.0f));
    }
}

namespace {
    // A point light's source radius softens it up close, like a spotlight's; one that is not positive falls back to 1.
    void testPointLightSourceRadiusIsPacked() {
        FrameLighting lighting;

        PlacedPointLight moon = pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, 100.0f);
        moon.light.sourceRadius = 4.0f;
        lighting.pointLights.push_back(moon);

        PlacedPointLight broken = moon;
        broken.light.sourceRadius = -2.0f;
        lighting.pointLights.push_back(broken);

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(nearlyEqual(data.points[0].radiance[3], 4.0f));
        assert(nearlyEqual(data.points[1].radiance[3], 1.0f));
        assert(!PointLight{}.castsShadows);
    }

    // The first packed point light that casts shadows gets the shadow map. Its slot is counted after skipped lights,
    // because the shader indexes the packed array.
    void testShadowedPointLightSlot() {
        FrameLighting lighting;
        lighting.pointLights.push_back(pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, 5.0f));

        PlacedPointLight skipped = pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
        skipped.light.castsShadows = true;
        lighting.pointLights.push_back(skipped);

        PlacedPointLight first = pointLightAt(Vec3{1.0f, 0.0f, 0.0f}, 5.0f);
        first.light.castsShadows = true;
        lighting.pointLights.push_back(first);

        PlacedPointLight second = pointLightAt(Vec3{2.0f, 0.0f, 0.0f}, 5.0f);
        second.light.castsShadows = true;
        lighting.pointLights.push_back(second);

        const LightUniformData data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[1] == 3);
        assert(data.counts[3] == 1);
        assert(nearlyEqual(data.points[1].positionRange[0], 1.0f));
    }
}

void testLightUniforms() {
    testPointLightSourceRadiusIsPacked();
    testShadowedPointLightSlot();
    testSpotLightSourceRadiusIsPacked();
    testSpotLightIsPacked();
    testSpotLightWithoutDirectionOrRangeIsSkipped();
    testSpotLightInnerConeIsClamped();
    testSpotLightsAreCapped();
    testAmbientAndEmptyCounts();
    testDirectionalLightIsFlippedAndPremultiplied();
    testZeroDirectionIsSkipped();
    testPointLightIsPacked();
    testPointLightWithoutRangeIsSkipped();
    testCameraPositionIsPacked();
    testCountsAreCapped();
}
