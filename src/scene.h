#pragma once

#include "mesh_instance.h"
#include "model.h"

#include <vector>

class Scene {
public:
    void add(const MeshInstance& object);

    void addModel(const Model& model, const Transform& placement = Transform{}, const Mat4& normalization = Mat4::identity(), const Vec3& rotationSpeed = Vec3{});

    std::vector<MeshInstance>& getObjects();
    const std::vector<MeshInstance>& getObjects() const;

private:
    std::vector<MeshInstance> objects;
};