#pragma once

#include "mesh.h"
#include "pixel.h"
#include "transform.h"
#include "material.h"

#include <memory>

struct MeshInstance {
    std::shared_ptr<const Mesh> mesh;
    Transform transform;
    Vec3 initialRotation{0.0f, 0.0f, 0.0f};
    Vec3 rotationSpeed{0.0f, 0.0f, 0.0f}; // Radians per second.
    Material material;

    explicit MeshInstance(std::shared_ptr<const Mesh> mesh);
};