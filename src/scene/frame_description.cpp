#include "scene/frame_description.h"
#include "math/transform.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"
#include "scene/still_draws.h"

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
void buildEntityDraws(const World& world, Entity entity, const ModelRenderer& renderer, float alpha, MotionHistory* history,
                      std::span<DrawItem> draws) {
    const Mat4 entityMatrix = getRenderTransform(world, entity, alpha).getMatrix();

    for (std::size_t part = 0; part < renderer.parts.size(); ++part) {
        const RenderPart& piece = renderer.parts[part];
        const Mat4 model = entityMatrix * piece.localTransform;
        DrawItem& draw = draws[part];
        draw = DrawItem{piece.mesh, model, piece.material, renderer.castsShadows, renderer.isStatic, BoundingSphere{},
                        history != nullptr ? history->previous(entity, part) : std::nullopt};
        draw.shadowOnly = renderer.shadowOnly;
        draw.shadowCascades = renderer.shadowCascades;
        draw.hidden = renderer.hidden;
        draw.followsGround = renderer.followsGround;
        draw.sway = renderer.sway;

        if (history != nullptr) {
            history->remember(entity, part, model);
        }
    }
}

/*
* With kept draws, the still entities' come first from them (StillDraws::update) and only the others are counted and
* built here, after them.
*/
FrameDescription buildFrame(World& world, const Camera& camera, float alpha, MotionHistory* history, JobSystem& jobs, std::size_t laterDraws,
                            StillDraws* still) {
    FrameDescription frame{.camera = camera, .lighting = collectLighting(world, alpha), .lightFocus = camera.getPosition()};

    world.each<Wind>([&](Entity, Wind& wind) {
        if (!frame.wind) {
            frame.wind = wind;
        }
    });
    world.each<WindAnchors>([&](Entity, WindAnchors& anchors) {
        if (!frame.windAnchors) {
            frame.windAnchors = anchors.anchors;
        }
    });
    const ComponentStorage<ModelRenderer>* renderers = world.storageOf<ModelRenderer>();

    if (renderers == nullptr) {
        frame.draws.reserve(laterDraws);
    } else {
        const std::vector<Entity>& entities = renderers->getEntities();
        const std::vector<ModelRenderer>& components = renderers->getComponents();
        const ComponentStorage<Transform>* transforms = world.storageOf<Transform>();
        std::vector<std::uint8_t> kept;
        std::size_t stillCount = 0;

        if (still != nullptr) {
            StillDraws::Update update = still->update(world, alpha, jobs);
            frame.draws = std::move(update.draws);
            frame.stillDraws = update.count;
            frame.builtStillDraws = std::move(update.built);
            frame.stillVersion = update.version;
            frame.stillPreviousVersion = update.previousVersion;
            kept = std::move(update.kept);
            stillCount = update.count;
        }

        std::vector<std::size_t> firstDraw(entities.size() + 1, stillCount);
        std::size_t slots = 0;

        for (std::size_t index = 0; index < entities.size(); ++index) {
            const bool drawn = (kept.empty() || kept[index] == 0) && components[index].visible && transforms != nullptr && transforms->contains(entities[index]);
            firstDraw[index + 1] = firstDraw[index] + (drawn ? components[index].parts.size() : 0);
            slots = std::max<std::size_t>(slots, entities[index].index + 1);
        }

        frame.draws.reserve(firstDraw.back() + laterDraws);
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

            buildEntityDraws(reading, entities[index], components[index], alpha, history,
                             std::span<DrawItem>{frame.draws}.subspan(firstDraw[index], components[index].parts.size()));
        });
    }

    if (history != nullptr) {
        history->endFrame();
    }

    return frame;
}

// A frame has few deforming meshes, so each draw looks through them.
void attachShapes(std::span<DrawItem> draws, const std::vector<DeformedMesh>& deformedMeshes) {
    for (DrawItem& draw : draws) {
        for (const DeformedMesh& deformed : deformedMeshes) {
            if (draw.mesh.index == deformed.mesh.index && draw.mesh.isValid()) {
                draw.shape = deformed.shape;
            }
        }
    }
}
