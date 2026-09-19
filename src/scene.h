#pragma once

#include "mesh_instance.h"

#include <vector>

class Scene {
public:
    void add(const MeshInstance& object);

    std::vector<MeshInstance>& getObjects();
    const std::vector<MeshInstance>& getObjects() const;

private:
    std::vector<MeshInstance> objects;
};