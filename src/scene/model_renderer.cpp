#include "scene/model_renderer.h"

#include <stdexcept>

ModelRenderer makeMeshRenderer(MeshHandle mesh, const Material& material) {
    if (!mesh.isValid()) {
        throw std::invalid_argument("Cannot render without a mesh");
    }

    ModelRenderer renderer;
    renderer.parts.push_back(RenderPart{mesh, material, Mat4::identity()});
    return renderer;
}
