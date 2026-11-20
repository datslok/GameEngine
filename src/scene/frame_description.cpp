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

// The renderer's choices carried into a part's draw; bounds, levels and the rest are attached later by the engine.
DrawItem rendererPartDraw(const ModelRenderer& renderer, std::size_t part, const Mat4& model) {
    const RenderPart& piece = renderer.parts[part];
    DrawItem draw;
    draw.mesh = piece.mesh;
    draw.model = model;
    draw.material = piece.material;
    draw.castsShadows = renderer.castsShadows;
    draw.isStatic = renderer.isStatic;
    draw.shadowOnly = renderer.shadowOnly;
    draw.shadowCascades = renderer.shadowCascades;
    draw.hidden = renderer.hidden;
    draw.followsGround = renderer.followsGround;
    draw.sway = renderer.sway;
    return draw;
}

/*
* Each kind's models' parts, in the plant's frame (their local transforms only), one run a kind in the looks' order.
* A handful of draws a kind, made every frame: cheaper than keeping them, since the engine attaches spans of the assets'
* levels to them, which a change of levels would leave dangling.
*/
void appendPlantedParts(FrameDescription& frame, const PlantingLooks& looks) {
    frame.plantedRegion = looks.region;

    for (const PlantedKindLook& kind : looks.kinds) {
        PlantedKindParts run{.layer = kind.layer, .kind = kind.kind, .first = frame.plantedParts.size(), .variant = kind.variant};

        for (const ModelRenderer& model : kind.models) {
            for (std::size_t part = 0; part < model.parts.size(); ++part) {
                frame.plantedParts.push_back(rendererPartDraw(model, part, model.parts[part].localTransform));
            }
        }

        run.count = frame.plantedParts.size() - run.first;
        frame.plantedKinds.push_back(run);
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
        draw = rendererPartDraw(renderer, part, model);
        draw.previousModel = history != nullptr ? history->previous(entity, part) : std::nullopt;

        if (history != nullptr) {
            history->remember(entity, part, model);
        }
    }
}

std::vector<DrawRange> joinDrawRanges(std::vector<DrawRange> ranges) {
    std::sort(ranges.begin(), ranges.end(), [](const DrawRange& a, const DrawRange& b) { return a.first < b.first; });
    std::vector<DrawRange> joined;

    for (const DrawRange& range : ranges) {
        if (range.count == 0) {
            continue;
        }

        if (!joined.empty() && range.first <= joined.back().first + joined.back().count) {
            const std::size_t end = std::max(joined.back().first + joined.back().count, range.first + range.count);
            joined.back().count = end - joined.back().first;
        } else {
            joined.push_back(range);
        }
    }

    return joined;
}

/*
* With kept draws, the still entities' come first from them (StillDraws::update) and only the moving ones it names are
* counted and built here, after them, so nothing walks every renderer; without, every renderer is walked.
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
    world.each<PlantingSource>([&](Entity, PlantingSource& source) {
        if (!frame.planting) {
            frame.planting = source.field;
            frame.plantingVersion = source.version;

            if (source.looks) {
                appendPlantedParts(frame, *source.looks);
            }
        }
    });
    const ComponentStorage<ModelRenderer>* renderers = world.storageOf<ModelRenderer>();

    if (renderers == nullptr) {
        frame.draws.reserve(laterDraws);
    } else {
        const std::vector<Entity>& entities = renderers->getEntities();
        const std::vector<ModelRenderer>& components = renderers->getComponents();
        const ComponentStorage<Transform>* transforms = world.storageOf<Transform>();
        std::vector<std::size_t> places;
        std::size_t stillCount = 0;

        if (still != nullptr) {
            StillDraws::Update update = still->update(world, alpha, jobs);
            frame.draws = std::move(update.draws);
            frame.stillDraws = update.count;
            frame.builtStillDraws = std::move(update.built);
            frame.stillVersion = update.version;
            frame.stillPreviousVersion = update.previousVersion;
            frame.stillCells = still->getCellCount();
            frame.stillChunkSlots = still->getChunkSlots();
            frame.stillEmptySlots = update.count - std::min(update.count, still->getFilledCount());
            places = std::move(update.moving);
            stillCount = update.count;
        } else {
            for (std::size_t index = 0; index < entities.size(); ++index) {
                if (components[index].isVisible() && transforms != nullptr && transforms->contains(entities[index])) {
                    places.push_back(index);
                }
            }
        }

        std::vector<std::size_t> firstDraw(places.size() + 1, stillCount);
        std::size_t slots = 0;

        for (std::size_t built = 0; built < places.size(); ++built) {
            firstDraw[built + 1] = firstDraw[built] + components[places[built]].parts.size();
            slots = std::max<std::size_t>(slots, entities[places[built]].index + 1);
        }

        frame.draws.reserve(firstDraw.back() + laterDraws);
        frame.draws.resize(firstDraw.back());

        if (history != nullptr) {
            history->reserveSlots(slots);
        }

        constexpr std::size_t entitiesPerPiece = 256;
        const World& reading = world;
        jobs.parallelFor(places.size(), entitiesPerPiece, [&](std::size_t built) {
            const std::size_t index = places[built];
            buildEntityDraws(reading, entities[index], components[index], alpha, history,
                             std::span<DrawItem>{frame.draws}.subspan(firstDraw[built], components[index].parts.size()));
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
