#include "scene/model_renderer.h"
#include "scene/still_draws.h"

#include <stdexcept>

ModelRenderer makeMeshRenderer(MeshHandle mesh, const Material& material) {
    if (!mesh.isValid()) {
        throw std::invalid_argument("Cannot render without a mesh");
    }

    ModelRenderer renderer;
    renderer.parts.push_back(RenderPart{mesh, material, Mat4::identity()});
    return renderer;
}

// Announced only when it changes, so setting what it already is costs the kept draws nothing.
void setRenderVisible(World& world, Entity entity, bool shown) {
    ModelRenderer* renderer = world.tryGet<ModelRenderer>(entity);

    if (renderer != nullptr && renderer->visible != shown) {
        renderer->visible = shown;
        markRenderChanged(world, entity);
    }
}

void setRenderStill(World& world, Entity entity, bool kept) {
    ModelRenderer* renderer = world.tryGet<ModelRenderer>(entity);

    if (renderer != nullptr && renderer->still != kept) {
        renderer->still = kept;
        markRenderChanged(world, entity);
    }
}
