#pragma once

#include "mesh.h"
#include "pixel.h"
#include "transform.h"

#include <memory>

struct MeshInstance {
    std::shared_ptr<const Mesh> mesh;
    Transform transform;
    Vec3 initialRotation{0.0f, 0.0f, 0.0f};
    Vec3 rotationSpeed{0.0f, 0.0f, 0.0f}; // Radians per second.
    Pixel colour{255, 255, 255};
    // Use the demo image when enabled; otherwise use plain colour.
    bool useTexture = false;

    explicit MeshInstance(std::shared_ptr<const Mesh> mesh);
};