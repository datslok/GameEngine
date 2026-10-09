#pragma once

#include "ecs/world.h"
#include "math/mat4.h"
#include "scene/asset_handles.h"
#include "scene/camera.h"
#include "scene/debug_draw.h"
#include "scene/lighting.h"
#include "scene/material.h"

#include <vector>

// A sphere around something, in world space. A negative radius means the size is unknown.
struct BoundingSphere {
    Vec3 centre{0.0f, 0.0f, 0.0f};
    float radius = -1.0f;
};

// One mesh to draw: which mesh, where (its full model matrix), and how it looks.
struct DrawItem {
    MeshHandle mesh;
    Mat4 model = Mat4::identity();
    Material material;
    bool castsShadows = true;

    // Filled in by the engine from the mesh's bounds (attachBoundingSpheres), so views can skip what they cannot see.
    BoundingSphere bounds;
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

    // Where the action is: when there are more lights than the renderer can use, the brightest ones here win.
    // The camera's position unless the game names a better point (a MOBA camera floats far above the ground it shows).
    Vec3 lightFocus{0.0f, 0.0f, 0.0f};

    // Wireframe shapes drawn over the scene (see DebugDraw), and lines on the screen itself in window pixels (overlays, text).
    std::vector<DebugLine> debugLines;
    std::vector<DebugLine> debugScreenLines;
};

// Describe the World as seen by camera, alpha (0..1) of the way between the last two ticks.
FrameDescription buildFrame(World& world, const Camera& camera, float alpha);
