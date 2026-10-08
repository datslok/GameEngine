#pragma once

#include "math/mat4.h"
#include "scene/material.h"
#include "scene/mesh.h"
#include "math/vec3.h"

#include <memory>
#include <vector>

struct ModelPart {
    std::shared_ptr<const Mesh> mesh;
    Material material;

    // Positions this part within the model.
    // Includes any parent transforms from the imported file.
    Mat4 transform = Mat4::identity();
};

struct ModelBounds {
    Vec3 minimum;
    Vec3 maximum;

    Vec3 centre() const;
    Vec3 size() const;
};

struct Model {
    std::vector<ModelPart> parts;

    ModelBounds getBounds() const;

    // Centre the model and scale its longest side to targetSize.
    Mat4 getNormalizationMatrix(float targetSize = 2.0f) const;
};