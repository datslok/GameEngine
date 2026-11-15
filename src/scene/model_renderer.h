#pragma once

#include "math/mat4.h"
#include "scene/asset_handles.h"
#include "scene/material.h"

#include <vector>

// One drawable piece of a model.
struct RenderPart {
    MeshHandle mesh;
    Material material;

    // Places the part relative to the entity's Transform. Includes any node transforms and normalization from the imported model.
    Mat4 localTransform = Mat4::identity();
};

/*
* Component: what to draw for an entity. A model with several meshes stays one entity with several parts, all moved by the entity's single Transform.
* Holds only handles and small values, so it is cheap to copy and could be sent over a network.
* Imported models are turned into one with AssetManager::makeModelRenderer.
*/
struct ModelRenderer {
    std::vector<RenderPart> parts;
    bool visible = true;

    // Whether it blocks light from shadow-casting lights. Off for things that surround their own light, like a glowing lamp shade.
    bool castsShadows = true;

    // Drawn into shadow maps only, never seen: a cheaper stand-in for something whose own castsShadows is off (the
    // forest's shadow leaves, fewer and bigger cards than the leaves seen, since every leaf card in the sun's cascades
    // was redrawn each frame the camera turned). Rays never meet it either.
    bool shadowOnly = false;

    // How many of the sun's cascades it casts into, counted from the nearest (0: all of them, and the far shadow map). A
    // small thing's shadow is only metres long, which the coarser cascades' texels cannot show, so the forest's
    // understorey casts into the first two only: 10,000 plants redrawn into every cascade as the camera turned.
    int shadowCascades = 0;

    // Part of the level that never moves (floors, walls): global illumination's probes move out of it at start-up, and
    // only things not marked static wake sleeping probes. Unmarked things count as moving, which costs only some probe
    // work, while a mover marked static would leave probes asleep beside it.
    bool isStatic = false;

    // Drawn by no view, casting no shadow and met by no ray, but part of the static level the sky visibility's rays see
    // (when isStatic): the forest's 2 m ground, walked on, while its terrain patches are what is drawn (scene/terrain.h).
    bool hidden = false;

    /*
    * Never moves and never changes (scene/still_draws.h): its draws are built once and kept from frame to frame, which
    * with the forest's 35,000 objects saved most of the CPU's frame. A game that does change one (its Transform, parts or
    * materials) calls markRenderChanged that frame; being hidden or shown, given more or fewer parts, or no longer still
    * are noticed without it. Debug builds check that kept matrices still match. Separate from isStatic: a tree is still
    * but not part of the level the probes and the sky bake treat as solid. No PreviousTransform needed.
    */
    bool still = false;
};

// Build a renderer with a single mesh.
ModelRenderer makeMeshRenderer(MeshHandle mesh, const Material& material = Material{});
