#pragma once

#include "scene/mesh_instance.h"
#include "scene/model.h"

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

    // Remember every transform before a simulation tick changes them.
    void savePreviousTransforms();

private:
    std::vector<MeshInstance> objects;
    std::vector<std::shared_ptr<ModelInstance>> modelInstances;
};