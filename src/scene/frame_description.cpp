#include "scene/frame_description.h"
#include "math/transform.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"

#include <algorithm>

std::optional<Mat4> MotionHistory::previous(Entity entity, std::size_t part) const {
    const std::vector<Record>& last = records[(frame - 1) & 1];

    if (entity.index >= last.size()) {
        return std::nullopt;
    }

    const Record& record = last[entity.index];

    if (record.frame != frame - 1 || record.generation != entity.generation || part >= record.parts.size()) {
        return std::nullopt;
    }

    return record.parts[part];
}

void MotionHistory::remember(Entity entity, std::size_t part, const Mat4& model) {
    std::vector<Record>& current = records[frame & 1];

    if (entity.index >= current.size()) {
        current.resize(entity.index + 1);
    }

    Record& record = current[entity.index];

    if (record.frame != frame) {
        record.frame = frame;
        record.generation = entity.generation;
        record.parts.clear();
    }

    if (part >= record.parts.size()) {
        record.parts.resize(part + 1, Mat4::identity());
    }

    record.parts[part] = model;
}

void MotionHistory::endFrame() {
    ++frame;
}

void MotionHistory::reserveSlots(std::size_t count) {
    for (std::vector<Record>& set : records) {
        if (set.size() < count) {
            set.resize(count);
        }
    }
}

/*
* Walks every entity that has something to draw and a place to draw it, in the order world.each walks them (the packed
* order of ModelRenderer). Each part of a model becomes its own draw, placed by the entity's interpolated transform times
* the part's local transform. First, one thread counts each entity's draws, which places every entity's draws in the
* list; then the entities are built on every core, each writing only its own draws and its own motion record, so the
* list is the same whatever order the threads finish in (with 20,000 objects, one thread took 12 ms).
*/
FrameDescription buildFrame(World& world, const Camera& camera, float alpha, MotionHistory* history, JobSystem& jobs) {
    FrameDescription frame{camera, collectLighting(world, alpha), {}, camera.getPosition(), 0.0f, {}, {}, {}, {}, 0, {}, {}, {}, 0};
    const ComponentStorage<ModelRenderer>* renderers = world.storageOf<ModelRenderer>();

    if (renderers != nullptr) {
        const std::vector<Entity>& entities = renderers->getEntities();
        const std::vector<ModelRenderer>& components = renderers->getComponents();
        std::vector<std::size_t> firstDraw(entities.size() + 1, 0);
        std::size_t slots = 0;

        for (std::size_t index = 0; index < entities.size(); ++index) {
            const bool drawn = components[index].visible && world.tryGet<Transform>(entities[index]) != nullptr;
            firstDraw[index + 1] = firstDraw[index] + (drawn ? components[index].parts.size() : 0);
            slots = std::max<std::size_t>(slots, entities[index].index + 1);
        }

        frame.draws.resize(firstDraw.back());

        if (history != nullptr) {
            history->reserveSlots(slots);
        }

        constexpr std::size_t entitiesPerPiece = 256;
        const World& reading = world;
        jobs.parallelFor(entities.size(), entitiesPerPiece, [&](std::size_t index) {
            if (firstDraw[index + 1] == firstDraw[index]) {
                return;
            }

            const Entity entity = entities[index];
            const ModelRenderer& modelRenderer = components[index];
            const Mat4 entityMatrix = getRenderTransform(reading, entity, alpha).getMatrix();

            for (std::size_t part = 0; part < modelRenderer.parts.size(); ++part) {
                const RenderPart& piece = modelRenderer.parts[part];
                const Mat4 model = entityMatrix * piece.localTransform;
                DrawItem& draw = frame.draws[firstDraw[index] + part];
                draw = DrawItem{piece.mesh, model, piece.material, modelRenderer.castsShadows, modelRenderer.isStatic, BoundingSphere{},
                                history != nullptr ? history->previous(entity, part) : std::nullopt};
                draw.shadowOnly = modelRenderer.shadowOnly;
                draw.shadowCascades = modelRenderer.shadowCascades;
                draw.hidden = modelRenderer.hidden;

                if (history != nullptr) {
                    history->remember(entity, part, model);
                }
            }
        });
    }

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
