#include "gameplay/flashlight.h"
#include "math/transform.h"
#include "scene/lighting.h"

/*
* The beam starts beside the eye and passes through the point straight ahead of it, so the middle of the lit patch is
* where you look whatever the distance to it.
*/
FlashlightPose flashlightInHand(const Camera& camera) {
    const Vec3 position = camera.getPosition() + camera.getRight() * flashlightHandRight - camera.getUp() * flashlightHandDown;
    const Vec3 aimPoint = camera.getPosition() + camera.getForward() * flashlightAimDistance;
    return FlashlightPose{position, aimPoint - position};
}

void Flashlight::create(World& world, bool on) {
    entity = world.create();
    world.add(entity, Transform{});

    if (on) {
        toggle(world);
    }
}

/*
* Each switch tells the light probes within the beam's reach, so its bounce comes and goes at once instead of over their
* memory (a scene event; the probes check where the light really changed).
*/
void Flashlight::toggle(World& world) {
    signalProbeEvent(world, probeEventAround(ProbeEventKind::LightChange, world.get<Transform>(entity).position, flashlightBeam.range));

    if (world.has<SpotLight>(entity)) {
        world.remove<SpotLight>(entity);
        return;
    }

    SpotLight beam = flashlightBeam;
    beam.direction = direction;
    world.add(entity, beam);
}

bool Flashlight::isOn(const World& world) const {
    return world.has<SpotLight>(entity);
}

void Flashlight::place(World& world, const FlashlightPose& pose) {
    world.get<Transform>(entity).position = pose.position;
    direction = pose.direction;

    if (SpotLight* beam = world.tryGet<SpotLight>(entity)) {
        beam->direction = direction;
    }
}

Entity Flashlight::getEntity() const {
    return entity;
}
