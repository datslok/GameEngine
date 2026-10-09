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
};

// Build a renderer with a single mesh.
ModelRenderer makeMeshRenderer(MeshHandle mesh, const Material& material = Material{});
