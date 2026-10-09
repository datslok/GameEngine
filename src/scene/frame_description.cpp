#include "scene/frame_description.h"
#include "math/transform.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"

/*
* Walks every entity that has something to draw and a place to draw it. Each part of a model becomes its own draw,
* placed by the entity's interpolated transform times the part's local transform.
*/
FrameDescription buildFrame(World& world, const Camera& camera, float alpha) {
    FrameDescription frame{camera, collectLighting(world, alpha), {}, camera.getPosition(), 0.0f, {}, {}};

    world.each<ModelRenderer, Transform>([&](Entity entity, ModelRenderer& modelRenderer, Transform&) {
        if (!modelRenderer.visible) {
            return;
        }

        const Mat4 entityMatrix = getRenderTransform(world, entity, alpha).getMatrix();

        for (const RenderPart& part : modelRenderer.parts) {
            frame.draws.push_back(DrawItem{part.mesh, entityMatrix * part.localTransform, part.material, modelRenderer.castsShadows, BoundingSphere{}});
        }
    });

    return frame;
}
