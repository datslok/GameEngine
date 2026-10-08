#include "scene/scene.h"

#include <iterator>
#include <stdexcept>
#include <utility>

void Scene::add(const MeshInstance& object)
{
    objects.push_back(object);

    // A new object has no earlier position to interpolate from.
    objects.back().previousTransform = objects.back().transform;
}

/*
* Called at the start of each simulation tick, so rendering can blend from these transforms to the ones the tick produces.
*/
void Scene::savePreviousTransforms() {
    for (MeshInstance& object : objects) {
        object.previousTransform = object.transform;
    }

    for (const auto& instance : modelInstances) {
        instance->previousTransform = instance->transform;
    }
}

std::vector<MeshInstance>& Scene::getObjects()
{
    return objects;
}

const std::vector<MeshInstance>& Scene::getObjects() const
{
    return objects;
}

const std::vector<std::shared_ptr<ModelInstance>>&Scene::getModelInstances() const {
    return modelInstances;
}

std::shared_ptr<ModelInstance> Scene::addModel(const Model& model, const Transform& placement, const Mat4& normalization, const Vec3& rotationSpeed) {
    auto instance = std::make_shared<ModelInstance>();

    instance->transform = placement;
    instance->previousTransform = placement;
    instance->normalization = normalization;
    instance->initialRotation = placement.rotation;
    instance->rotationSpeed = rotationSpeed;

    // Validate and prepare all parts before changing the scene.
    std::vector<MeshInstance> parts;
    parts.reserve(model.parts.size());

    for (const ModelPart& part : model.parts) {
        if (!part.mesh) {
            throw std::invalid_argument(
                "Cannot add a model part with a null mesh"
            );
        }

        MeshInstance object{part.mesh};

        object.material = part.material;
        object.localTransform = part.transform;
        object.modelInstance = instance;

        parts.push_back(std::move(object));
    }

    // An empty model produces a handle but adds nothing to the scene.
    if (parts.empty()) {
        return instance;
    }

    objects.reserve(objects.size() + parts.size());
    modelInstances.reserve(modelInstances.size() + 1);

    objects.insert(
        objects.end(),
        std::make_move_iterator(parts.begin()),
        std::make_move_iterator(parts.end())
    );

    modelInstances.push_back(instance);

    return instance;
}