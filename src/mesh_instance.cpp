#include "mesh_instance.h"

#include <stdexcept>
#include <utility>

MeshInstance::MeshInstance(std::shared_ptr<const Mesh> mesh):
    mesh(std::move(mesh)){
    if (!this->mesh) {
        throw std::invalid_argument(
            "MeshInstance requires a mesh"
        );
    }
}

Mat4 MeshInstance::getModelMatrix() const {
    const Mat4 partMatrix = transform.getMatrix() * localTransform;

    if (modelInstance) {
        return modelInstance->getMatrix() * partMatrix;
    }

    return partMatrix;
}