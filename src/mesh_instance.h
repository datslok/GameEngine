#pragma once

#include "mesh.h"
#include "pixel.h"
#include "transform.h"
#include "material.h"
#include "mat4.h"

#include <memory>

struct MeshInstance {
    std::shared_ptr<const Mesh> mesh;
    Transform transform;
    // Positions this mesh within an imported model.
    Mat4 localTransform = Mat4::identity();
    Mat4 getModelMatrix() const;
    Vec3 initialRotation{0.0f, 0.0f, 0.0f};
    Vec3 rotationSpeed{0.0f, 0.0f, 0.0f}; // Radians per second.
    Material material;

    explicit MeshInstance(std::shared_ptr<const Mesh> mesh);
};