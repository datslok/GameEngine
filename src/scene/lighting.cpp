#include "scene/lighting.h"

#include "math/transform.h"
#include "scene/interpolation.h"

/*
* Collect lights once per frame, so the renderer can send them to the GPU in one go.
* Point light positions are interpolated exactly like meshes, so a light carried by a moving object does not jitter against it.
* A point light or spotlight without a Transform has no position, so it is skipped.
*/
FrameLighting collectLighting(World& world, float alpha) {
    FrameLighting lighting;

    world.each<AmbientLight>([&](Entity, AmbientLight& ambient) {
        lighting.ambient = lighting.ambient + ambient.colour;
    });

    world.each<SkyBackground>([&](Entity, SkyBackground& sky) {
        lighting.background = lighting.background + sky.colour;
    });

    world.each<DirectionalLight>([&](Entity, DirectionalLight& light) {
        lighting.directionalLights.push_back(light);
    });

    world.each<PointLight, Transform>([&](Entity entity, PointLight& light, Transform&) {
        lighting.pointLights.push_back(PlacedPointLight{getRenderTransform(world, entity, alpha).position, light, entity});
    });

    world.each<SpotLight, Transform>([&](Entity entity, SpotLight& light, Transform&) {
        lighting.spotLights.push_back(PlacedSpotLight{getRenderTransform(world, entity, alpha).position, light, entity});
    });

    world.each<ProbeVolume>([&](Entity, ProbeVolume& volume) {
        lighting.probeVolumes.push_back(volume);
    });

    world.each<Atmosphere>([&](Entity, Atmosphere& atmosphere) {
        if (!lighting.atmosphere) {
            lighting.atmosphere = atmosphere;
        }
    });

    world.each<ProbeEvent>([&](Entity, ProbeEvent& event) {
        lighting.probeEvents.push_back(event);
    });

    return lighting;
}

// Each event is an entity of its own, so a game can signal several in a frame.
void signalProbeEvent(World& world, const ProbeEvent& event) {
    world.add(world.create(), event);
}

// Destroying inside the walk is queued until it ends.
void clearProbeEvents(World& world) {
    world.each<ProbeEvent>([&](Entity entity, ProbeEvent&) {
        world.destroy(entity);
    });
}
