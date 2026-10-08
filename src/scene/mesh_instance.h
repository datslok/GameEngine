#pragma once

#include "scene/mesh.h"
#include "core/pixel.h"
#include "math/transform.h"
#include "scene/material.h"
#include "math/mat4.h"
#include "scene/model_instance.h"

#include <memory>

struct MeshInstance {
    std::shared_ptr<const Mesh> mesh;
    Transform transform;
    // The transform at the start of the latest simulation tick, used for render interpolation.
    Transform previousTransform;
    // Positions this mesh within an imported model.
    Mat4 localTransform = Mat4::identity();
    Mat4 getModelMatrix() const;
    // The model matrix blended between the previous and current tick, for rendering.
    Mat4 getInterpolatedModelMatrix(float alpha) const;
    Vec3 initialRotation{0.0f, 0.0f, 0.0f};
    Vec3 rotationSpeed{0.0f, 0.0f, 0.0f}; // Radians per second.
    Material material;
    // Null for standalone meshes; shared by parts of an imported model.
    std::shared_ptr<ModelInstance> modelInstance;
    explicit MeshInstance(std::shared_ptr<const Mesh> mesh);
    bool visible = true;
};