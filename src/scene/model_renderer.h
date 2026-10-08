#pragma once

#include "math/mat4.h"
#include "scene/material.h"
#include "scene/mesh.h"
#include "scene/model.h"

#include <memory>
#include <vector>

// One drawable piece of a model.
struct RenderPart {
    std::shared_ptr<const Mesh> mesh;
    Material material;

    // Places the part relative to the entity's Transform. Includes any node transforms and normalization from the imported model.
    Mat4 localTransform = Mat4::identity();
};

/*
* Component: what to draw for an entity. A model with several meshes stays one entity with several parts, all moved by the entity's single Transform.
*/
struct ModelRenderer {
    std::vector<RenderPart> parts;
    bool visible = true;
};

// Build a renderer for an imported model. normalization is applied below each part's own transform.
ModelRenderer makeModelRenderer(const Model& model, const Mat4& normalization = Mat4::identity());

// Build a renderer with a single mesh.
ModelRenderer makeMeshRenderer(std::shared_ptr<const Mesh> mesh, const Material& material = Material{});
