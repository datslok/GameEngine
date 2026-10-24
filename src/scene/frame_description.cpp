#include "scene/frame_description.h"
#include "core/hash.h"
#include "math/transform.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"

std::optional<Mat4> MotionHistory::previous(Entity entity, std::size_t part) const {
    const auto found = last.find(Key{entity.index, entity.generation, part});
    return found != last.end() ? std::optional<Mat4>{found->second} : std::nullopt;
}

void MotionHistory::remember(Entity entity, std::size_t part, const Mat4& model) {
    current[Key{entity.index, entity.generation, part}] = model;
}

void MotionHistory::endFrame() {
    last = std::move(current);
    current.clear();
}

// The slot and generation side by side in one number, the part mixed in, then the bits spread (core/hash.h).
std::size_t MotionHistory::KeyHash::operator()(const Key& key) const {
    const std::uint64_t entity = (std::uint64_t{key.generation} << 32) | key.index;
    return static_cast<std::size_t>(mixBits(entity ^ mixBits(key.part)));
}

/*
* Walks every entity that has something to draw and a place to draw it. Each part of a model becomes its own draw,
* placed by the entity's interpolated transform times the part's local transform.
*/
FrameDescription buildFrame(World& world, const Camera& camera, float alpha, MotionHistory* history) {
    FrameDescription frame{camera, collectLighting(world, alpha), {}, camera.getPosition(), 0.0f, {}, {}, {}};

    world.each<ModelRenderer, Transform>([&](Entity entity, ModelRenderer& modelRenderer, Transform&) {
        if (!modelRenderer.visible) {
            return;
        }

        const Mat4 entityMatrix = getRenderTransform(world, entity, alpha).getMatrix();

        for (std::size_t index = 0; index < modelRenderer.parts.size(); ++index) {
            const RenderPart& part = modelRenderer.parts[index];
            const Mat4 model = entityMatrix * part.localTransform;
            frame.draws.push_back(DrawItem{part.mesh, model, part.material, modelRenderer.castsShadows, modelRenderer.isStatic, BoundingSphere{},
                                           history != nullptr ? history->previous(entity, index) : std::nullopt});

            if (history != nullptr) {
                history->remember(entity, index, model);
            }
        }
    });

    if (history != nullptr) {
        history->endFrame();
    }

    return frame;
}

// A frame has few deforming meshes, so each draw looks through them.
void attachShapes(std::vector<DrawItem>& draws, const std::vector<DeformedMesh>& deformedMeshes) {
    for (DrawItem& draw : draws) {
        for (const DeformedMesh& deformed : deformedMeshes) {
            if (draw.mesh.index == deformed.mesh.index && draw.mesh.isValid()) {
                draw.shape = deformed.shape;
            }
        }
    }
}
