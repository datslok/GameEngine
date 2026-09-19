#pragma once

#include "mesh.h"
#include "pixel.h"
#include "transform.h"

#include <memory>

struct MeshInstance {
    std::shared_ptr<const Mesh> mesh;
    Transform transform;
    Pixel colour{255, 255, 255};

    explicit MeshInstance(std::shared_ptr<const Mesh> mesh);
};