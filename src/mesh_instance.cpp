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

Mat4 MeshInstance::getInterpolatedModelMatrix(float alpha) const {
    const Mat4 partMatrix =
        interpolate(previousTransform, transform, alpha).getMatrix() * localTransform;

    if (modelInstance) {
        return modelInstance->getInterpolatedMatrix(alpha) * partMatrix;
    }

    return partMatrix;
}