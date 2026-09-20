#pragma once

#include "mat4.h"
#include "material.h"
#include "mesh.h"

#include <memory>
#include <vector>

struct ModelPart {
    std::shared_ptr<const Mesh> mesh;
    Material material;

    // Positions this part within the model.
    // Includes any parent transforms from the imported file.
    Mat4 transform = Mat4::identity();
};

struct Model {
    std::vector<ModelPart> parts;
};