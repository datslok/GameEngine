#include "scene/interpolation.h"

/*
* Run at the start of every tick, so rendering can blend from these transforms to the ones the tick produces.
*/
void savePreviousTransforms(World& world) {
    world.each<PreviousTransform, Transform>([](Entity, PreviousTransform& previous, Transform& current) {
        previous.transform = current;
    });
}

Transform getRenderTransform(const World& world, Entity entity, float alpha) {
    const Transform& current = world.get<Transform>(entity);
    const PreviousTransform* previous = world.tryGet<PreviousTransform>(entity);

    if (!previous) {
        return current;
    }

    return interpolate(previous->transform, current, alpha);
}
