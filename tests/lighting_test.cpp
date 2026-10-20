#include "ecs/world.h"
#include "math/transform.h"
#include "scene/interpolation.h"
#include "scene/light.h"
#include "scene/lighting.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }

    bool nearlyEqual(const Vec3& actual, const Vec3& expected) {
        return nearlyEqual(actual.x, expected.x) && nearlyEqual(actual.y, expected.y) && nearlyEqual(actual.z, expected.z);
    }

    void testEmptyWorldHasNoLight() {
        World world;
        const FrameLighting lighting = collectLighting(world, 0.0f);

        assert(nearlyEqual(lighting.ambient, Vec3{0.0f, 0.0f, 0.0f}));
        assert(nearlyEqual(lighting.background, Vec3{0.0f, 0.0f, 0.0f}));
        assert(lighting.directionalLights.empty());
        assert(lighting.pointLights.empty());
        assert(!lighting.probeCascades.has_value());
    }

    // A game places light probes as it places lights: its ProbeCascades (the first, if several) reaches the renderer.
    void testProbeCascadesAreCopied() {
        World world;
        world.add(world.create(), ProbeCascades{false, Vec3{-3.0f, 0.5f, 2.0f}});
        world.add(world.create(), ProbeCascades{});

        const FrameLighting lighting = collectLighting(world, 0.0f);

        assert(lighting.probeCascades.has_value());
        assert(!lighting.probeCascades->followCamera);
        assert(nearlyEqual(lighting.probeCascades->centre, Vec3{-3.0f, 0.5f, 2.0f}));
    }

    // What shows where nothing is drawn: black by default, the sky's colour when a game sets one (several add up).
    void testSkyBackgroundIsCollected() {
        World world;
        world.add(world.create(), SkyBackground{Vec3{0.5f, 0.6f, 0.9f}});
        world.add(world.create(), SkyBackground{Vec3{0.1f, 0.1f, 0.1f}});

        const FrameLighting lighting = collectLighting(world, 0.0f);

        assert(nearlyEqual(lighting.background, Vec3{0.6f, 0.7f, 1.0f}));
    }

    void testAmbientLightsAddUp() {
        World world;
        world.add(world.create(), AmbientLight{Vec3{0.1f, 0.2f, 0.3f}});
        world.add(world.create(), AmbientLight{Vec3{0.1f, 0.1f, 0.1f}});

        const FrameLighting lighting = collectLighting(world, 0.0f);

        assert(nearlyEqual(lighting.ambient, Vec3{0.2f, 0.3f, 0.4f}));
    }

    void testDirectionalLightsAreCopied() {
        World world;
        world.add(world.create(), DirectionalLight{Vec3{1.0f, -2.0f, -1.0f}, Vec3{1.0f, 0.5f, 0.25f}, 0.8f});

        const FrameLighting lighting = collectLighting(world, 0.0f);

        assert(lighting.directionalLights.size() == 1);
        const DirectionalLight& light = lighting.directionalLights[0];
        assert(nearlyEqual(light.direction, Vec3{1.0f, -2.0f, -1.0f}));
        assert(nearlyEqual(light.colour, Vec3{1.0f, 0.5f, 0.25f}));
        assert(nearlyEqual(light.intensity, 0.8f));
    }

    // A moving light is drawn where its entity is drawn, between the last two ticks.
    void testPointLightPositionIsInterpolated() {
        World world;
        const Entity torch = world.create();

        Transform current;
        current.position = Vec3{2.0f, 0.0f, 0.0f};
        Transform previous;
        previous.position = Vec3{0.0f, 0.0f, 0.0f};

        world.add(torch, current);
        world.add(torch, PreviousTransform{previous});
        world.add(torch, PointLight{Vec3{1.0f, 1.0f, 1.0f}, 1.0f, 5.0f});

        // A point light with nowhere to be is skipped.
        world.add(world.create(), PointLight{});

        const FrameLighting lighting = collectLighting(world, 0.25f);

        assert(lighting.pointLights.size() == 1);
        assert(nearlyEqual(lighting.pointLights[0].position, Vec3{0.5f, 0.0f, 0.0f}));
        assert(nearlyEqual(lighting.pointLights[0].light.range, 5.0f));

        // The entity travels with the light, so the renderer can recognise it next frame.
        assert(lighting.pointLights[0].entity == torch);
    }
}

namespace {
    // Spotlights are placed like point lights, and keep their beam settings.
    void testSpotLightIsCollectedWithItsPosition() {
        World world;
        const Entity flashlight = world.create();

        Transform current;
        current.position = Vec3{0.0f, 4.0f, 0.0f};
        Transform previous;
        previous.position = Vec3{0.0f, 0.0f, 0.0f};

        world.add(flashlight, current);
        world.add(flashlight, PreviousTransform{previous});
        world.add(flashlight, SpotLight{Vec3{1.0f, 1.0f, 1.0f}, 2.0f, 20.0f, Vec3{0.0f, 0.0f, -1.0f}, 0.2f, 0.4f});

        // A spotlight with nowhere to be is skipped.
        world.add(world.create(), SpotLight{});

        const FrameLighting lighting = collectLighting(world, 0.5f);

        assert(lighting.spotLights.size() == 1);
        const PlacedSpotLight& placed = lighting.spotLights[0];
        assert(nearlyEqual(placed.position, Vec3{0.0f, 2.0f, 0.0f}));
        assert(placed.entity == flashlight);
        assert(nearlyEqual(placed.light.direction, Vec3{0.0f, 0.0f, -1.0f}));
        assert(nearlyEqual(placed.light.range, 20.0f));
        assert(nearlyEqual(placed.light.innerAngle, 0.2f));
        assert(nearlyEqual(placed.light.outerAngle, 0.4f));
    }

    /*
    * A game signals a big change to the light probes for one frame: every event signalled since the last frame reaches
    * that frame's lighting, in the order signalled, and clearing after the frame is built leaves none for the next.
    */
    void testProbeEventsLastOneFrame() {
        World world;
        signalProbeEvent(world, ProbeEvent{});
        signalProbeEvent(world, probeEventAround(ProbeEventKind::GeometryChange, Vec3{1.0f, 2.0f, 3.0f}, 0.5f));

        const FrameLighting lighting = collectLighting(world, 0.0f);
        assert(lighting.probeEvents.size() == 2);
        assert(lighting.probeEvents[0].kind == ProbeEventKind::LightChange && lighting.probeEvents[0].everywhere);
        const ProbeEvent& local = lighting.probeEvents[1];
        assert(local.kind == ProbeEventKind::GeometryChange && !local.everywhere);
        assert(nearlyEqual(local.region.minimum, Vec3{0.5f, 1.5f, 2.5f}) && nearlyEqual(local.region.maximum, Vec3{1.5f, 2.5f, 3.5f}));

        clearProbeEvents(world);
        assert(collectLighting(world, 0.0f).probeEvents.empty());
        int left = 0;
        world.each<ProbeEvent>([&](Entity, ProbeEvent&) {
            ++left;
        });
        assert(left == 0);
    }
}

void testLighting() {
    testSpotLightIsCollectedWithItsPosition();
    testEmptyWorldHasNoLight();
    testAmbientLightsAddUp();
    testSkyBackgroundIsCollected();
    testDirectionalLightsAreCopied();
    testPointLightPositionIsInterpolated();
    testProbeCascadesAreCopied();
    testProbeEventsLastOneFrame();
}
