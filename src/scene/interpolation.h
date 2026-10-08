#pragma once

#include "ecs/world.h"
#include "math/transform.h"

/*
* Component: the entity's Transform at the start of the latest simulation tick.
* Only entities that move continuously need one. Entities without it are drawn exactly at their Transform, which is also right for things that teleport.
*/
struct PreviousTransform {
    Transform transform;
};

// System: remember every moving entity's Transform before a tick changes it.
void savePreviousTransforms(World& world);

// The transform to draw an entity with: blended between the last two ticks when it has a PreviousTransform.
Transform getRenderTransform(const World& world, Entity entity, float alpha);
