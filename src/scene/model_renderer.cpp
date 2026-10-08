#include "scene/model_renderer.h"

#include <stdexcept>
#include <utility>

/*
* Fold the normalization into each part once, here, instead of multiplying it in for every part on every frame.
* Validates every part first, so a bad model throws before anything is built.
*/
ModelRenderer makeModelRenderer(const Model& model, const Mat4& normalization) {
    ModelRenderer renderer;
    renderer.parts.reserve(model.parts.size());

    for (const ModelPart& part : model.parts) {
        if (!part.mesh) {
            throw std::invalid_argument("Cannot render a model part with a null mesh");
        }

        renderer.parts.push_back(RenderPart{
            part.mesh,
            part.material,
            normalization * part.transform
        });
    }

    return renderer;
}

ModelRenderer makeMeshRenderer(std::shared_ptr<const Mesh> mesh, const Material& material) {
    if (!mesh) {
        throw std::invalid_argument("Cannot render a null mesh");
    }

    ModelRenderer renderer;
    renderer.parts.push_back(RenderPart{std::move(mesh), material, Mat4::identity()});
    return renderer;
}
