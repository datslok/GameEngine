#pragma once

#include "mesh_instance.h"
#include "model.h"

#include <vector>

class Scene {
public:
    void add(const MeshInstance& object);
    std::shared_ptr<ModelInstance> addModel(const Model& model, const Transform& placement = Transform{},
        const Mat4& normalization = Mat4::identity(), const Vec3& rotationSpeed = Vec3{});
    std::vector<MeshInstance>& getObjects();
    const std::vector<MeshInstance>& getObjects() const;
    const std::vector<std::shared_ptr<ModelInstance>>&
    getModelInstances() const;

private:
    std::vector<MeshInstance> objects;
    std::vector<std::shared_ptr<ModelInstance>> modelInstances;
};