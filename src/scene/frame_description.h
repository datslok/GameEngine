#pragma once

#include "ecs/world.h"
#include "math/mat4.h"
#include "scene/asset_handles.h"
#include "scene/camera.h"
#include "scene/lighting.h"
#include "scene/material.h"

#include <vector>

// One mesh to draw: which mesh, where (its full model matrix), and how it looks.
struct DrawItem {
    MeshHandle mesh;
    Mat4 model = Mat4::identity();
    Material material;
    bool castsShadows = true;
};

/*
* Everything the renderer needs for one frame, as plain data with no GPU types: the camera, the lights and every mesh to draw.
* Handing the renderer the whole frame at once lets it draw the scene more than once (a shadow map from the light, then the
* camera's view) and skip what the camera cannot see, which a draw-one-mesh-now interface cannot do.
*/
struct FrameDescription {
    Camera camera;
    FrameLighting lighting;
    std::vector<DrawItem> draws;
};

// Describe the World as seen by camera, alpha (0..1) of the way between the last two ticks.
FrameDescription buildFrame(World& world, const Camera& camera, float alpha);
