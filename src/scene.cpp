#include "scene.h"

#include <stdexcept>
#include <utility>

void Scene::add(const MeshInstance& object)
{
    objects.push_back(object);
}

std::vector<MeshInstance>& Scene::getObjects()
{
    return objects;
}

const std::vector<MeshInstance>& Scene::getObjects() const
{
    return objects;
}

void Scene::addModel(const Model& model, const Transform& placement, const Mat4& normalization, const Vec3& rotationSpeed) {
    // Build the instances before modifying the scene.
    std::vector<MeshInstance> instances;
    instances.reserve(model.parts.size());

    for (const ModelPart& part : model.parts) {
        if (!part.mesh) {
            throw std::invalid_argument(
                "Cannot add a model part with a null mesh"
            );
        }

        MeshInstance object{part.mesh};

        object.material = part.material;
        object.localTransform = normalization * part.transform;

        object.transform = placement;
        object.initialRotation = placement.rotation;
        object.rotationSpeed = rotationSpeed;

        instances.push_back(std::move(object));
    }

    objects.insert(
        objects.end(),
        instances.begin(),
        instances.end()
    );
}