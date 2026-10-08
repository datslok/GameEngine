#pragma once

#include "ecs/world.h"
#include "math/vec3.h"
#include "scene/light.h"

#include <vector>

// A point light together with where it is this frame.
struct PlacedPointLight {
    Vec3 position;
    PointLight light;
};

// Every light the renderer needs for one frame, with no GPU types.
struct FrameLighting {
    Vec3 ambient{0.0f, 0.0f, 0.0f};
    std::vector<DirectionalLight> directionalLights;
    std::vector<PlacedPointLight> pointLights;
};

// Gather the World's lights for drawing at alpha (0..1) between the last two ticks.
FrameLighting collectLighting(World& world, float alpha);
