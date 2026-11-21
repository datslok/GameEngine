#pragma once

#include "ecs/world.h"
#include "math/mat4.h"
#include "scene/asset_handles.h"
#include "scene/material.h"
#include "scene/wind.h"

#include <utility>
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

    // Drawn at all (shown), and kept from frame to frame (still: see below).
    bool isVisible() const { return visible; }
    bool isStill() const { return still; }

    /*
    * Chosen before the renderer joins a world: callable only on a value about to be moved in, so one already in a
    * world changes only through setRenderVisible and setRenderStill, which announce it. What keeps the still draws
    * (scene/still_draws.h) looking only at what changed, never at every renderer.
    */
    ModelRenderer withVisible(bool shown) && {
        visible = shown;
        return std::move(*this);
    }

    ModelRenderer withStill(bool kept) && {
        still = kept;
        return std::move(*this);
    }

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

    // The nearest cascade it casts into (0 the first): a plant's stand-in starts where its seen mesh, casting into the
    // cascades before, hands its shadow on (shadowHandedOn), so the coarse stand-in never shadows the leaves seen close
    // up in polygons (docs/research/FOLIAGESHADOWS.md, Horizon Zero Dawn's split).
    int shadowFromCascade = 0;

    // Its shadow goes on past its last cascade (shadowCascades) in another caster (the stand-in): its last cascade takes
    // it whole, not thinned out across its edge.
    bool shadowHandedOn = false;

    // Part of the level that never moves (floors, walls): global illumination's probes move out of it at start-up, and
    // only things not marked static wake sleeping probes. Unmarked things count as moving, which costs only some probe
    // work, while a mover marked static would leave probes asleep beside it.
    bool isStatic = false;

    // Drawn by no view, casting no shadow and met by no ray, but part of the static level the sky visibility's rays see
    // (when isStatic): the forest's 2 m ground, walked on, while its terrain patches are what is drawn (scene/terrain.h).
    bool hidden = false;

    /*
    * Follows the ground (scene/ground_follow.h): its meshes stand on y = 0 of their own frame, and the vertex shaders
    * lift each vertex by the terrain's height at its place, so one flat patch of grass serves every spot of lumpy land.
    * Its bounds are lifted to match; no ray meets it.
    */
    bool followsGround = false;

    // How it sways in the wind (scene/wind.h; Still by default): every vertex shader bends it by its spring.
    WindSway sway{};

    /*
    * A liana the GPU bends (scene/liana_shape.h): one past its record in the frame's LianaShapes (0 for everything
    * else). Its mesh is the template tube, which the vertex shaders put on the record's catenary and sway; its Transform
    * only places the template's box round the liana (lianaPose), for its bounds and level. No ray meets it.
    */
    std::uint32_t lianaShape = 0;

private:
    friend void setRenderVisible(World& world, Entity entity, bool shown);
    friend void setRenderStill(World& world, Entity entity, bool kept);

    bool visible = true;

    /*
    * Never moves and never changes (scene/still_draws.h): its draws are built once and kept from frame to frame, which
    * with the forest's 35,000 objects saved most of the CPU's frame. A game that does change one (its Transform, parts or
    * materials) calls markRenderChanged that frame; hiding, showing and stilling announce themselves. Debug builds check
    * that kept matrices and parts still match. Separate from isStatic: a tree is still but not part of the level the
    * probes and the sky bake treat as solid. No PreviousTransform needed.
    */
    bool still = false;
};

// Shows or hides an entity's renderer, announced (markRenderChanged) when it changes. Nothing without a renderer.
void setRenderVisible(World& world, Entity entity, bool shown);

// Makes an entity's renderer still or moving, announced when it changes. Nothing without a renderer.
void setRenderStill(World& world, Entity entity, bool kept);

// Build a renderer with a single mesh.
ModelRenderer makeMeshRenderer(MeshHandle mesh, const Material& material = Material{});
