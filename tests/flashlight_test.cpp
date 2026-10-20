#include "gameplay/flashlight.h"
#include "math/transform.h"
#include "scene/lighting.h"

#include <cassert>
#include <cmath>

namespace {
    // A camera at a slant, so right, up and forward are not the world's axes.
    Camera slantedCamera() {
        return Camera{Vec3{3.0f, 1.7f, -2.0f}, Vec3{-4.0f, 0.5f, -9.0f}, Vec3{0.0f, 1.0f, 0.0f}, 1.2f, 16.0f / 9.0f, 0.1f, 500.0f};
    }

    /*
    * In the right hand, a little to the right of the eye and below it (along the camera's up, which is the world's, so
    * straight down however the view tilts), its beam through the point straight ahead.
    */
    void testTheFlashlightIsHeldInTheRightHand() {
        const Camera camera = slantedCamera();
        const FlashlightPose pose = flashlightInHand(camera);
        const Vec3 fromEye = pose.position - camera.getPosition();

        assert(std::abs(fromEye.dot(camera.getRight()) - flashlightHandRight) < 1e-5f);
        assert(std::abs(fromEye.dot(camera.getUp()) + flashlightHandDown) < 1e-5f);

        const Vec3 aimPoint = camera.getPosition() + camera.getForward() * flashlightAimDistance;
        assert(pose.direction.normalized().dot((aimPoint - pose.position).normalized()) > 0.99999f);
    }

    // Off removes the light (no seat spent on it), on puts the same beam back; the entity stays either way.
    void testTogglingAddsAndRemovesTheBeam() {
        World world;
        Flashlight flashlight;
        flashlight.create(world, false);
        const Entity entity = flashlight.getEntity();

        assert(world.isAlive(entity) && world.has<Transform>(entity));
        assert(!flashlight.isOn(world) && !world.has<SpotLight>(entity));

        flashlight.toggle(world);
        assert(flashlight.isOn(world));
        const SpotLight& beam = world.get<SpotLight>(entity);
        assert(beam.intensity == flashlightBeam.intensity && beam.range == flashlightBeam.range);

        // Swung around with the view, so its bounce light must follow it (global illumination's fast lights).
        assert(beam.fast);

        flashlight.toggle(world);
        assert(!flashlight.isOn(world) && world.isAlive(entity));
        clearProbeEvents(world);

        Flashlight lit;
        lit.create(world, true);
        assert(lit.isOn(world));
    }

    // Placing moves the light and aims its beam; switched off, it still follows, so it comes back on where it should be.
    void testPlacingMovesTheLightAndAimsTheBeam() {
        World world;
        Flashlight flashlight;
        flashlight.create(world, true);

        const FlashlightPose pose{Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.0f, -0.5f, -1.0f}};
        flashlight.place(world, pose);
        const Entity entity = flashlight.getEntity();
        const Vec3 position = world.get<Transform>(entity).position;
        const Vec3 direction = world.get<SpotLight>(entity).direction;
        assert(position.x == 1.0f && position.y == 2.0f && position.z == 3.0f);
        assert(direction.x == 0.0f && direction.y == -0.5f && direction.z == -1.0f);

        flashlight.toggle(world);
        flashlight.place(world, FlashlightPose{Vec3{5.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}});
        assert(world.get<Transform>(entity).position.x == 5.0f);
        flashlight.toggle(world);
        assert(world.get<SpotLight>(entity).direction.x == 1.0f);
    }
}

namespace {
    /*
    * Each switch, on or off, is a scene event for the light probes: a light change over the beam's reach round the
    * flashlight, so its bounce comes and goes at once (render/gpu/ddgi.h, advanceProbeRestarts).
    */
    void testEachSwitchIsAProbeEvent() {
        World world;
        Flashlight flashlight;
        flashlight.create(world, false);
        flashlight.place(world, FlashlightPose{Vec3{4.0f, 1.5f, -2.0f}, Vec3{0.0f, 0.0f, -1.0f}});
        assert(collectLighting(world, 1.0f).probeEvents.empty());

        for (int switches = 0; switches < 2; ++switches) {
            flashlight.toggle(world);
            const std::vector<ProbeEvent> events = collectLighting(world, 1.0f).probeEvents;
            assert(events.size() == 1);
            assert(events[0].kind == ProbeEventKind::LightChange && !events[0].everywhere);
            const float reach = flashlightBeam.range * 0.99f;
            assert(events[0].region.contains(Vec3{4.0f + reach, 1.5f - reach, -2.0f + reach}));
            assert(events[0].region.contains(Vec3{4.0f - reach, 1.5f + reach, -2.0f - reach}));
            clearProbeEvents(world);
        }
    }
}

void testFlashlight() {
    testEachSwitchIsAProbeEvent();
    testTheFlashlightIsHeldInTheRightHand();
    testTogglingAddsAndRemovesTheBeam();
    testPlacingMovesTheLightAndAimsTheBeam();
}
