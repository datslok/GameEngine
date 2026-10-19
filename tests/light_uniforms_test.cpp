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

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(nearlyEqual(data.ambient, 0.2f, 0.2f, 0.2f, 0.0f));
        assert(data.counts[0] == 0 && data.counts[1] == 0 && data.counts[2] == 0 && data.counts[3] == 0);
    }

    // The shader wants the direction towards the light, normalised, and colour already multiplied by intensity.
    void testDirectionalLightIsFlippedAndPremultiplied() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.0f, -2.0f, 0.0f}, Vec3{1.0f, 0.5f, 0.5f}, 2.0f});

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[0] == 1);
        assert(nearlyEqual(data.directional[0].toLight, 0.0f, 1.0f, 0.0f, 0.0f));
        assert(nearlyEqual(data.directional[0].radiance, 2.0f, 1.0f, 1.0f, 0.0f));
    }

    // A zero direction cannot be normalised, so it must not take a slot.
    void testZeroDirectionIsSkipped() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f});
        lighting.directionalLights.push_back(DirectionalLight{Vec3{1.0f, 0.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, 1.0f});

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[0] == 1);
        assert(nearlyEqual(data.directional[0].toLight, -1.0f, 0.0f, 0.0f, 0.0f));
    }

    void testPointLightIsPacked() {
        FrameLighting lighting;
        lighting.pointLights.push_back(PlacedPointLight{Vec3{1.0f, 2.0f, 3.0f}, PointLight{Vec3{1.0f, 1.0f, 1.0f}, 3.0f, 6.0f}});

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[1] == 1);
        assert(nearlyEqual(data.points[0].positionRange, 1.0f, 2.0f, 3.0f, 6.0f));
        assert(nearlyEqual(data.points[0].radiance, 3.0f, 3.0f, 3.0f, 1.0f)); // w: source radius, 1 by default
    }

    // The shader divides by range, so a light without a positive range would turn pixels into NaN.
    void testPointLightWithoutRangeIsSkipped() {
        FrameLighting lighting;
        lighting.pointLights.push_back(pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, 0.0f));
        lighting.pointLights.push_back(pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, -1.0f));

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[1] == 0);
    }

    // The shader works relative to the camera (it sits at the origin there), so light positions arrive measured from it.
    // Directions do not depend on where anything is, so they are unchanged.
    void testPositionsAreRelativeToTheCamera() {
        FrameLighting lighting;
        lighting.pointLights.push_back(pointLightAt(Vec3{1.0f, 2.0f, 3.0f}, 5.0f));

        PlacedSpotLight spot{};
        spot.position = Vec3{-4.0f, 6.0f, 0.0f};
        spot.light.direction = Vec3{0.0f, -1.0f, 0.0f};
        spot.light.range = 8.0f;
        lighting.spotLights.push_back(spot);

        const GpuLights data = packLighting(lighting, Vec3{0.5f, 1.0f, -2.0f});

        assert(nearlyEqual(data.points[0].positionRange, 0.5f, 1.0f, 5.0f, 5.0f));
        assert(nearlyEqual(data.spots[0].positionRange, -4.5f, 5.0f, 2.0f, 8.0f));
        assert(nearlyEqual(data.spots[0].directionCosOuter[1], -1.0f));
    }

    // Lights past the limits are dropped; the first ones collected win.
    void testCountsAreCapped() {
        FrameLighting lighting;

        for (int i = 0; i < 6; ++i) {
            lighting.directionalLights.push_back(DirectionalLight{});
        }

        for (int i = 0; i < maxPointLights + 6; ++i) {
            lighting.pointLights.push_back(pointLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, 5.0f));
        }

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        // The storage buffer has room for far more lights than the 64 seated by default (light_priority.h).
        assert(maxPointLights == 1024 && defaultPointSeats == 64);
        assert(data.counts[0] == maxDirectionalLights);
        assert(data.counts[1] == maxPointLights);
        assert(data.points.size() == static_cast<std::size_t>(maxPointLights));
        assert(nearlyEqual(data.points[maxPointLights - 1].positionRange, 1023.0f, 0.0f, 0.0f, 5.0f));
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

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

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

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.counts[2] == 0);
    }

    // An inner cone wider than the outer one would make the smooth edge run backwards, so it is clamped to the outer cone.
    void testSpotLightInnerConeIsClamped() {
        FrameLighting lighting;
        lighting.spotLights.push_back(spotLightAt(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f, 0.6f, 0.3f));

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(nearlyEqual(data.spots[0].radianceCosInner[3], std::cos(0.3f)));
        assert(nearlyEqual(data.spots[0].directionCosOuter[3], std::cos(0.3f)));
    }

    void testSpotLightsAreCapped() {
        FrameLighting lighting;

        for (int i = 0; i < maxSpotLights + 2; ++i) {
            lighting.spotLights.push_back(spotLightAt(Vec3{static_cast<float>(i), 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f, 0.2f, 0.4f));
        }

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(maxSpotLights == 256 && defaultSpotSeats == 8);
        assert(data.counts[2] == maxSpotLights);
        assert(data.spots.size() == static_cast<std::size_t>(maxSpotLights));
        assert(nearlyEqual(data.spots[maxSpotLights - 1].positionRange, 255.0f, 0.0f, 0.0f, 20.0f));
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

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(nearlyEqual(data.spots[0].sourceRadius, 3.0f));
        assert(nearlyEqual(data.spots[1].sourceRadius, 1.0f));
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

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(nearlyEqual(data.points[0].radiance[3], 4.0f));
        assert(nearlyEqual(data.points[1].radiance[3], 1.0f));
    }
}

namespace {
    // A light fading in or out of its seat has its brightness scaled on the CPU, so the shader needs no extra field.
    void testFadeScalesRadiance() {
        FrameLighting lighting;
        PlacedPointLight point = pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, 5.0f);
        point.fade = 0.25f;
        lighting.pointLights.push_back(point);

        PlacedSpotLight spot = spotLightAt(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f, 0.2f, 0.4f);
        spot.fade = 0.5f;
        lighting.spotLights.push_back(spot);

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(nearlyEqual(data.points[0].radiance[0], 0.25f));
        assert(nearlyEqual(data.spots[0].radianceCosInner[0], 1.0f)); // colour 1 x intensity 2 x fade 0.5
        assert(PlacedPointLight{}.fade == 1.0f && PlacedSpotLight{}.shadowFade == 1.0f);
    }
}

namespace {
    // packLighting knows nothing of shadows: every light starts with none (tile -1, strength 0), and attachShadows
    // (shadow_map.h) fills in the ones the shadow plan gave tiles. One record per light the shader gets, no padding.
    void testLightsStartUnshadowed() {
        FrameLighting lighting;
        lighting.directionalLights.push_back(DirectionalLight{});
        lighting.pointLights.push_back(pointLightAt(Vec3{0.0f, 0.0f, 0.0f}, 5.0f));
        lighting.spotLights.push_back(spotLightAt(Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}, 20.0f, 0.2f, 0.4f));

        const GpuLights data = packLighting(lighting, Vec3{0.0f, 0.0f, 0.0f});

        assert(data.directional.size() == 1 && data.points.size() == 1 && data.spots.size() == 1);
        assert(data.directional[0].shadowTile == -1);
        assert(data.points[0].firstShadowTile == -1 && data.points[0].shadowStrength == 0.0f && data.points[0].emitterRadius == 0.0f);
        assert(data.spots[0].shadowTile == -1 && data.spots[0].shadowStrength == 0.0f && data.spots[0].emitterRadius == 0.0f);
    }
}

void testLightUniforms() {
    testLightsStartUnshadowed();
    testFadeScalesRadiance();
    testPointLightSourceRadiusIsPacked();
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
    testPositionsAreRelativeToTheCamera();
    testCountsAreCapped();
}
