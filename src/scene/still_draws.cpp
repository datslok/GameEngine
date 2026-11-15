#include "scene/still_draws.h"
#include "math/transform.h"
#include "scene/deforming_mesh.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"

#include <span>
#include <stdexcept>
#include <string>

void markRenderChanged(World& world, Entity entity) {
    if (!world.has<RenderChanged>(entity)) {
        world.addLater(entity, RenderChanged{});
    }
}

// Removing inside the walk is queued until it ends.
void clearRenderChanged(World& world) {
    world.each<RenderChanged>([&](Entity entity, RenderChanged&) {
        world.remove<RenderChanged>(entity);
    });
}

namespace {
    // A still draw: built without a history, last frame's matrix its own.
    void buildStillEntity(const World& world, Entity entity, const ModelRenderer& renderer, float alpha, std::span<DrawItem> draws) {
        buildEntityDraws(world, entity, renderer, alpha, nullptr, draws);

        for (DrawItem& draw : draws) {
            draw.previousModel = draw.model;
        }
    }
}

/*
* One walk over the renderers marks the still ones (drawn, placed, not deforming) and checks each against its entry: a
* new one, another generation in its slot or another number of parts, or a count that differs from what is kept (one
* gone or no longer still), and they are all built again; otherwise only the marked ones, in place. The storages are
* looked up once, since a lookup by type per entity cost more than the rest of the walk.
*/
StillDraws::Update StillDraws::update(const World& world, float alpha, JobSystem& jobs) {
    Update result;
    const ComponentStorage<ModelRenderer>* renderers = world.storageOf<ModelRenderer>();

    if (renderers == nullptr) {
        return result;
    }

    const std::vector<Entity>& entities = renderers->getEntities();
    const std::vector<ModelRenderer>& components = renderers->getComponents();
    const ComponentStorage<Transform>* transforms = world.storageOf<Transform>();
    const ComponentStorage<DeformingMesh>* deforming = world.storageOf<DeformingMesh>();
    const ComponentStorage<RenderChanged>* changes = world.storageOf<RenderChanged>();
    result.kept.assign(entities.size(), 0);

    bool rebuild = !valid || !holding;
    std::size_t stillEntities = 0;
    std::vector<std::size_t> changed;

    for (std::size_t index = 0; index < entities.size(); ++index) {
        const Entity entity = entities[index];
        const ModelRenderer& renderer = components[index];

        if (!renderer.still || !renderer.visible || transforms == nullptr || !transforms->contains(entity) || (deforming != nullptr && deforming->contains(entity))) {
            continue;
        }

        result.kept[index] = 1;
        ++stillEntities;

        if (rebuild) {
            continue;
        }

        const Entry* entry = entity.index < byIndex.size() ? &byIndex[entity.index] : nullptr;

        if (entry == nullptr || !entry->present || entry->generation != entity.generation || entry->parts != renderer.parts.size()) {
            rebuild = true;
        } else if (changes != nullptr && changes->contains(entity)) {
            changed.push_back(index);
        }
    }

    if (rebuild || stillEntities != order.size()) {
        buildAll(world, result.kept, alpha, jobs);
        result.built.push_back(DrawRange{0, count});
    } else {
        for (const std::size_t index : changed) {
            const Entry& entry = byIndex[entities[index].index];
            buildStillEntity(world, entities[index], components[index], alpha, std::span<DrawItem>{draws}.subspan(entry.first, entry.parts));
            result.built.push_back(DrawRange{entry.first, entry.parts});
        }
    }

    // Last frame's moving and terrain draws after them go.
    draws.resize(count);
    result.draws = std::move(draws);
    draws = {};
    result.count = count;
    holding = false;
    valid = true;
    return result;
}

void StillDraws::buildAll(const World& world, const std::vector<std::uint8_t>& kept, float alpha, JobSystem& jobs) {
    const ComponentStorage<ModelRenderer>* renderers = world.storageOf<ModelRenderer>();
    const std::vector<Entity>& entities = renderers->getEntities();
    const std::vector<ModelRenderer>& components = renderers->getComponents();

    for (const Entity entity : order) {
        byIndex[entity.index].present = false;
    }

    order.clear();
    std::vector<std::size_t> firstDraw(entities.size() + 1, 0);

    for (std::size_t index = 0; index < entities.size(); ++index) {
        const std::size_t parts = kept[index] != 0 ? components[index].parts.size() : 0;
        firstDraw[index + 1] = firstDraw[index] + parts;

        if (kept[index] == 0) {
            continue;
        }

        const Entity entity = entities[index];

        if (entity.index >= byIndex.size()) {
            byIndex.resize(static_cast<std::size_t>(entity.index) + 1);
        }

        byIndex[entity.index] = Entry{entity.generation, static_cast<std::uint32_t>(firstDraw[index]), static_cast<std::uint32_t>(parts), true};
        order.push_back(entity);
    }

    count = firstDraw.back();
    draws.clear();
    draws.resize(count);

    constexpr std::size_t entitiesPerPiece = 256;
    jobs.parallelFor(entities.size(), entitiesPerPiece, [&](std::size_t index) {
        if (kept[index] != 0) {
            buildStillEntity(world, entities[index], components[index], alpha,
                             std::span<DrawItem>{draws}.subspan(firstDraw[index], components[index].parts.size()));
        }
    });
}

void StillDraws::giveBack(std::vector<DrawItem> given) {
    if (given.size() < count) {
        valid = false;
        return;
    }

    draws = std::move(given);
    holding = true;
}

void StillDraws::invalidate() {
    valid = false;
}

/*
* Each kept entity's matrices are worked out again as buildFrame works them out, so they match bit for bit unless the
* entity moved; one marked and not built yet is skipped, as is one gone (the next frame builds them all anyway).
*/
void StillDraws::verify(const World& world, std::size_t checks) {
    if (!holding || !valid || order.empty()) {
        return;
    }

    for (std::size_t check = 0; check < checks && check < order.size(); ++check) {
        verifyNext = (verifyNext + 1) % order.size();
        const Entity entity = order[verifyNext];
        const ModelRenderer* renderer = world.isAlive(entity) ? world.tryGet<ModelRenderer>(entity) : nullptr;

        if (renderer == nullptr || !world.has<Transform>(entity) || world.has<RenderChanged>(entity)) {
            continue;
        }

        const Entry& entry = byIndex[entity.index];

        if (entry.parts != renderer->parts.size()) {
            continue;
        }

        const Mat4 entityMatrix = getRenderTransform(world, entity, 1.0f).getMatrix();

        for (std::size_t part = 0; part < renderer->parts.size(); ++part) {
            if (!(entityMatrix * renderer->parts[part].localTransform == draws[entry.first + part].model)) {
                throw std::logic_error("A still entity (ModelRenderer::still) moved without markRenderChanged: entity " + std::to_string(entity.index));
            }
        }
    }
}

std::size_t StillDraws::getCount() const {
    return count;
}
