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