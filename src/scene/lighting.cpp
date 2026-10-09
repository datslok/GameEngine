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

    world.each<DirectionalLight>([&](Entity, DirectionalLight& light) {
        lighting.directionalLights.push_back(light);
    });

    world.each<PointLight, Transform>([&](Entity entity, PointLight& light, Transform&) {
        lighting.pointLights.push_back(PlacedPointLight{getRenderTransform(world, entity, alpha).position, light});
    });

    world.each<SpotLight, Transform>([&](Entity entity, SpotLight& light, Transform&) {
        lighting.spotLights.push_back(PlacedSpotLight{getRenderTransform(world, entity, alpha).position, light});
    });

    return lighting;
}
