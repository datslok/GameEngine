#pragma once

#include "ecs/world.h"
#include "math/vec3.h"
#include "scene/light.h"

#include <vector>

// A point light together with where it is this frame, and whose it is, so the renderer can recognise it next frame
// (Entity{} for a light that belongs to no entity).
struct PlacedPointLight {
    Vec3 position;
    PointLight light;
    Entity entity{};
};

// A spotlight together with where it is this frame, and whose it is.
struct PlacedSpotLight {
    Vec3 position;
    SpotLight light;
    Entity entity{};
};

// Every light the renderer needs for one frame, with no GPU types.
struct FrameLighting {
    Vec3 ambient{0.0f, 0.0f, 0.0f};
    std::vector<DirectionalLight> directionalLights;
    std::vector<PlacedPointLight> pointLights;
    std::vector<PlacedSpotLight> spotLights;
};

// Gather the World's lights for drawing at alpha (0..1) between the last two ticks.
FrameLighting collectLighting(World& world, float alpha);
