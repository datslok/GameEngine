#include "math/frustum.h"
#include "render/gpu/light_priority.h"
#include "render/gpu/light_uniforms.h"
#include "render/gpu/shadow_map.h"
#include "scene/camera.h"
#include "scene/lighting.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {
    // A camera at the origin looking down -Z, and the space it sees.
    Frustum makeTestView() {
        const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, Vec3{0.0f, 0.0f, -1.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f};
        return Frustum::fromClipMatrix(camera.getProjectionMatrix() * camera.getViewMatrix(), ClipDepth::NegativeOneToOne);
    }

    const Vec3 focus{0.0f, 0.0f, -10.0f};

    PlacedPointLight pointLight(const Vec3& position, std::uint32_t entityIndex, float intensity = 1.0f) {
        PlacedPointLight placed{position, PointLight{}, Entity{entityIndex, 1}};
        placed.light.intensity = intensity;
        placed.light.range = 5.0f;
        return placed;
    }

    PlacedSpotLight spotLight(const Vec3& position, std::uint32_t entityIndex) {
        PlacedSpotLight placed{position, SpotLight{}, Entity{entityIndex, 1}};
        placed.light.range = 5.0f;
        return placed;
    }

    bool containsLight(const std::vector<PlacedPointLight>& lights, std::uint32_t entityIndex) {
        return std::any_of(lights.begin(), lights.end(), [&](const PlacedPointLight& placed) {
            return placed.entity.index == entityIndex;
        });
    }

    int countShadowed(const std::vector<PlacedPointLight>& lights) {
        return static_cast<int>(std::count_if(lights.begin(), lights.end(), [](const PlacedPointLight& placed) {
            return placed.light.castsShadows;
        }));
    }

    bool isShadowed(const std::vector<PlacedPointLight>& lights, std::uint32_t entityIndex) {
        return std::any_of(lights.begin(), lights.end(), [&](const PlacedPointLight& placed) {
            return placed.entity.index == entityIndex && placed.light.castsShadows;
        });
    }

    // Brightness at the focus falls off with distance and grows with intensity and colour, and never quite reaches zero,
    // so lights that are out of reach of the focus still rank by how far away they are instead of tying.
    void testImportanceFollowsBrightnessAtTheFocus() {
        const Vec3 white{1.0f, 1.0f, 1.0f};
        const float closeBy = lightImportance(Vec3{0.0f, 0.0f, -8.0f}, white, 1.0f, 1.0f, focus);
        const float farther = lightImportance(Vec3{0.0f, 0.0f, -4.0f}, white, 1.0f, 1.0f, focus);
        const float bright = lightImportance(Vec3{0.0f, 0.0f, -8.0f}, white, 3.0f, 1.0f, focus);
        const float dimColour = lightImportance(Vec3{0.0f, 0.0f, -8.0f}, Vec3{0.5f, 0.2f, 0.1f}, 1.0f, 1.0f, focus);
        const float veryFar = lightImportance(Vec3{0.0f, 0.0f, 90.0f}, white, 1.0f, 1.0f, focus);

        assert(closeBy > farther);
        assert(std::abs(bright - 3.0f * closeBy) < 1e-6f);
        assert(std::abs(dimColour - 0.5f * closeBy) < 1e-6f);
        assert(veryFar > 0.0f);

        // The source radius softens it up close, as in the shader: 1 / (d^2 + r^2).
        assert(std::abs(lightImportance(focus, white, 1.0f, 2.0f, focus) - 0.25f) < 1e-6f);
    }

    // With more point lights than the shader has seats for, the ones nearest the focus win, whatever order they were collected in.
    void testNearestPointLightsGetTheSeats() {
        FrameLighting lighting;

        const std::uint32_t lightCount = maxPointLights + 6;

        for (std::uint32_t i = 0; i < lightCount; ++i) {
            // Further from the focus the earlier they are collected.
            lighting.pointLights.push_back(pointLight(Vec3{0.0f, 0.0f, -10.0f - static_cast<float>(lightCount - i)}, i + 1));
        }

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});

        assert(static_cast<int>(result.lighting.pointLights.size()) == maxPointLights);

        for (std::uint32_t i = 0; i < 6; ++i) {
            assert(!containsLight(result.lighting.pointLights, i + 1));
        }

        assert(containsLight(result.lighting.pointLights, lightCount));
        assert(result.history.litPoints.size() == static_cast<std::size_t>(maxPointLights));
    }

    // A light whose whole range sphere is outside the view cannot light anything on screen, so it takes no seat,
    // however close to the focus it is.
    void testLightsOutOfViewAreSkipped() {
        FrameLighting lighting;
        lighting.pointLights.push_back(pointLight(Vec3{0.0f, 0.0f, 10.0f}, 1));  // behind the camera, range 5
        lighting.pointLights.push_back(pointLight(Vec3{0.0f, 0.0f, 3.0f}, 2));   // behind, but its range reaches into view
        lighting.spotLights.push_back(spotLight(Vec3{0.0f, 0.0f, 10.0f}, 3));

        const PrioritizedLighting result = prioritizeLights(lighting, Vec3{0.0f, 0.0f, 8.0f}, makeTestView(), LightHistory{});

        assert(result.lighting.pointLights.size() == 1);
        assert(result.lighting.pointLights[0].entity.index == 2);
        assert(result.lighting.spotLights.empty());

        // How many were in the running, for the debug readout.
        assert(result.pointLightsInView == 1);
    }

    // Lights the shader cannot draw take no seat either.
    void testUndrawableLightsAreSkipped() {
        FrameLighting lighting;
        PlacedPointLight noRange = pointLight(focus, 1);
        noRange.light.range = 0.0f;
        lighting.pointLights.push_back(noRange);
        lighting.directionalLights.push_back(DirectionalLight{Vec3{0.0f, 0.0f, 0.0f}});

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});

        assert(result.lighting.pointLights.empty());
        assert(result.lighting.directionalLights.empty());
    }

    // Shadows go to the most important lights that cast them; the other seated lights keep lighting, unshadowed.
    void testShadowsGoToTheMostImportantCasters() {
        FrameLighting lighting;
        const std::uint32_t lightCount = maxShadowedPointLights + 4;

        for (std::uint32_t i = 0; i < lightCount; ++i) {
            lighting.pointLights.push_back(pointLight(Vec3{0.0f, 0.0f, -10.0f - static_cast<float>(i)}, i + 1));
        }

        lighting.pointLights[0].light.castsShadows = false; // the nearest one asked for no shadows

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});

        assert(result.lighting.pointLights.size() == lightCount);
        assert(countShadowed(result.lighting.pointLights) == maxShadowedPointLights);
        assert(!isShadowed(result.lighting.pointLights, 1));

        for (std::uint32_t i = 2; i <= static_cast<std::uint32_t>(maxShadowedPointLights) + 1; ++i) {
            assert(isShadowed(result.lighting.pointLights, i));
        }

        assert(result.history.shadowedPoints.size() == static_cast<std::size_t>(maxShadowedPointLights));

        // Shadow planning then gives tiles to exactly those.
        const Camera camera{Vec3{0.0f, 0.0f, 0.0f}, focus, Vec3{0.0f, 1.0f, 0.0f}, 1.0f, 1.5f, 0.1f, 100.0f};
        const ShadowPlan plan = planShadows(result.lighting, camera);

        for (std::size_t slot = 0; slot < result.lighting.pointLights.size(); ++slot) {
            const bool hasTiles = plan.uniforms.pointTiles[slot / 4][slot % 4] >= 0;
            assert(hasTiles == result.lighting.pointLights[slot].light.castsShadows);
        }
    }

    // Hysteresis: a light that had a shadow last frame keeps it against a rival that is only a little brighter, and gives
    // it up to one that is clearly brighter, so two similar lights do not trade a shadow back and forth.
    void testShadowedLightKeepsItsShadowAgainstSimilarRivals() {
        FrameLighting first;
        const std::uint32_t rival = maxShadowedPointLights + 1;

        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(maxShadowedPointLights); ++i) {
            first.pointLights.push_back(pointLight(focus, i + 1, 1.0f));
        }

        first.pointLights.push_back(pointLight(focus, rival, 0.5f));
        const PrioritizedLighting before = prioritizeLights(first, focus, makeTestView(), LightHistory{});
        assert(!isShadowed(before.lighting.pointLights, rival));

        // The rival becomes a little brighter than the shadowed ones (all at the focus).
        FrameLighting slightly = first;
        slightly.pointLights.back().light.intensity = 1.1f;
        const PrioritizedLighting kept = prioritizeLights(slightly, focus, makeTestView(), before.history);
        assert(isShadowed(kept.lighting.pointLights, 1));
        assert(!isShadowed(kept.lighting.pointLights, rival));

        // Without last frame's history it would have won.
        const PrioritizedLighting fresh = prioritizeLights(slightly, focus, makeTestView(), LightHistory{});
        assert(isShadowed(fresh.lighting.pointLights, rival));

        // Clearly brighter takes the shadow.
        FrameLighting clearly = first;
        clearly.pointLights.back().light.intensity = 2.0f;
        const PrioritizedLighting taken = prioritizeLights(clearly, focus, makeTestView(), before.history);
        assert(isShadowed(taken.lighting.pointLights, rival));
        assert(countShadowed(taken.lighting.pointLights) == maxShadowedPointLights);
    }

    // Seats work the same way: a light that was lit last frame stays lit against a slightly more important newcomer.
    void testLitLightKeepsItsSeatAgainstSimilarRivals() {
        FrameLighting lighting;

        const std::uint32_t newcomer = maxPointLights + 1;

        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(maxPointLights); ++i) {
            lighting.pointLights.push_back(pointLight(Vec3{0.0f, 0.0f, -10.0f}, i + 1, 1.0f));
        }

        lighting.pointLights.push_back(pointLight(Vec3{0.0f, 0.0f, -10.0f}, newcomer, 0.5f));
        const PrioritizedLighting before = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});
        assert(!containsLight(before.lighting.pointLights, newcomer));

        lighting.pointLights.back().light.intensity = 1.1f;
        const PrioritizedLighting after = prioritizeLights(lighting, focus, makeTestView(), before.history);
        assert(!containsLight(after.lighting.pointLights, newcomer));
    }

    // A light without an entity (built by hand, not collected from a World) is never mistaken for last frame's.
    void testLightsWithoutEntityAreNeverIncumbents() {
        FrameLighting lighting;

        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(maxShadowedPointLights); ++i) {
            lighting.pointLights.push_back(pointLight(focus, i + 1, 1.1f));
        }

        // Dimmer than the others, so it gets a shadow only if it is wrongly given last frame's advantage.
        PlacedPointLight anonymous = pointLight(focus, 0, 1.0f);
        anonymous.entity = Entity{};
        lighting.pointLights.push_back(anonymous);

        LightHistory history;
        history.shadowedPoints.push_back(Entity{});

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), history);

        for (const PlacedPointLight& placed : result.lighting.pointLights) {
            assert(placed.light.castsShadows == (placed.entity != Entity{}));
        }
    }

    // Spotlights compete for their seats the same way. Every seated spotlight fits in the atlas, so each keeps its shadow.
    void testNearestSpotLightsGetTheSeats() {
        FrameLighting lighting;

        // Light i + 1 is 1.5 * i from the focus, collected in reverse so order cannot be what decides.
        for (std::uint32_t i = 10; i-- > 0;) {
            lighting.spotLights.push_back(spotLight(Vec3{0.0f, 0.0f, -10.0f - 1.5f * static_cast<float>(i)}, i + 1));
        }

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});

        assert(static_cast<int>(result.lighting.spotLights.size()) == maxSpotLights);

        for (const PlacedSpotLight& placed : result.lighting.spotLights) {
            assert(placed.entity.index >= 1 && placed.entity.index <= 8);
            assert(placed.light.castsShadows);
        }

        assert(result.history.litSpots.size() == static_cast<std::size_t>(maxSpotLights));
        assert(result.history.shadowedSpots.size() == static_cast<std::size_t>(maxSpotLights));
    }

    // Directional lights have no position, so the brightest ones win.
    void testBrightestDirectionalLightsGetTheSeats() {
        FrameLighting lighting;

        for (int i = 0; i < 6; ++i) {
            lighting.directionalLights.push_back(DirectionalLight{Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f}, static_cast<float>(i)});
        }

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});

        assert(static_cast<int>(result.lighting.directionalLights.size()) == maxDirectionalLights);

        for (const DirectionalLight& light : result.lighting.directionalLights) {
            assert(light.intensity >= 2.0f);
        }
    }

    // The ambient light is not ranked; it passes straight through.
    void testAmbientPassesThrough() {
        FrameLighting lighting;
        lighting.ambient = Vec3{0.1f, 0.2f, 0.3f};

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});

        assert(result.lighting.ambient.x == 0.1f && result.lighting.ambient.y == 0.2f && result.lighting.ambient.z == 0.3f);
    }
}

namespace {
    // A light a game marks with a higher priority gets a seat before any lower one, however far from the focus it is.
    void testHigherPriorityWinsASeat() {
        FrameLighting lighting;

        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(maxPointLights); ++i) {
            lighting.pointLights.push_back(pointLight(focus, i + 1));
        }

        PlacedPointLight marked = pointLight(Vec3{0.0f, 0.0f, -60.0f}, 100, 0.1f);
        marked.light.priority = 1;
        lighting.pointLights.push_back(marked);

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});

        assert(static_cast<int>(result.lighting.pointLights.size()) == maxPointLights);
        assert(containsLight(result.lighting.pointLights, 100));
    }

    // The same for shadows: the marked light gets one, and the others share what is left.
    void testHigherPriorityWinsAShadow() {
        FrameLighting lighting;

        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(maxShadowedPointLights); ++i) {
            lighting.pointLights.push_back(pointLight(focus, i + 1));
        }

        PlacedPointLight marked = pointLight(Vec3{0.0f, 0.0f, -30.0f}, 100, 0.1f);
        marked.light.priority = 2;
        lighting.pointLights.push_back(marked);

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});

        assert(isShadowed(result.lighting.pointLights, 100));
        assert(countShadowed(result.lighting.pointLights) == maxShadowedPointLights);
    }

    // Last frame's advantage only decides between lights of equal priority: a marked newcomer takes its shadow at once.
    void testPriorityBeatsLastFramesChoice() {
        FrameLighting lighting;

        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(maxShadowedPointLights); ++i) {
            lighting.pointLights.push_back(pointLight(focus, i + 1));
        }

        const PrioritizedLighting before = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});

        PlacedPointLight marked = pointLight(focus, 100, 0.5f);
        marked.light.priority = 1;
        lighting.pointLights.push_back(marked);

        const PrioritizedLighting after = prioritizeLights(lighting, focus, makeTestView(), before.history);
        assert(isShadowed(after.lighting.pointLights, 100));
    }

    // Priority does not bring back a light that cannot reach anything on screen.
    void testPriorityDoesNotOverrideTheView() {
        FrameLighting lighting;
        PlacedPointLight behind = pointLight(Vec3{0.0f, 0.0f, 10.0f}, 1);
        behind.light.priority = 5;
        lighting.pointLights.push_back(behind);

        PlacedSpotLight spotBehind = spotLight(Vec3{0.0f, 0.0f, 10.0f}, 2);
        spotBehind.light.priority = 5;
        lighting.spotLights.push_back(spotBehind);

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});
        assert(result.lighting.pointLights.empty());
        assert(result.lighting.spotLights.empty());
    }

    // Spotlights follow the same rule.
    void testSpotLightPriority() {
        FrameLighting lighting;

        for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(maxSpotLights); ++i) {
            lighting.spotLights.push_back(spotLight(focus, i + 1));
        }

        PlacedSpotLight marked = spotLight(Vec3{0.0f, 0.0f, -40.0f}, 100);
        marked.light.priority = 1;
        lighting.spotLights.push_back(marked);

        const PrioritizedLighting result = prioritizeLights(lighting, focus, makeTestView(), LightHistory{});

        assert(static_cast<int>(result.lighting.spotLights.size()) == maxSpotLights);
        assert(result.lighting.spotLights[0].entity.index == 100); // most important first
    }

    // Lights default to the same priority, so ranking stays by brightness unless a game asks.
    void testPriorityDefaultsToZero() {
        assert(PointLight{}.priority == 0);
        assert(SpotLight{}.priority == 0);
    }
}

void testLightPriority() {
    testHigherPriorityWinsASeat();
    testHigherPriorityWinsAShadow();
    testPriorityBeatsLastFramesChoice();
    testPriorityDoesNotOverrideTheView();
    testSpotLightPriority();
    testPriorityDefaultsToZero();
    testImportanceFollowsBrightnessAtTheFocus();
    testNearestPointLightsGetTheSeats();
    testLightsOutOfViewAreSkipped();
    testUndrawableLightsAreSkipped();
    testShadowsGoToTheMostImportantCasters();
    testShadowedLightKeepsItsShadowAgainstSimilarRivals();
    testLitLightKeepsItsSeatAgainstSimilarRivals();
    testLightsWithoutEntityAreNeverIncumbents();
    testNearestSpotLightsGetTheSeats();
    testBrightestDirectionalLightsGetTheSeats();
    testAmbientPassesThrough();
}
