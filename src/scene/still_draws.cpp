#include "scene/still_draws.h"
#include "math/transform.h"
#include "scene/deforming_mesh.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"

#include <algorithm>
#include <set>
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

DrawItem emptyDrawSlot() {
    DrawItem draw;
    draw.castsShadows = false;
    draw.hidden = true;
    return draw;
}

bool isEmptyDrawSlot(const DrawItem& draw) {
    return !draw.mesh.isValid();
}

namespace {
    // A still draw: built without a history, last frame's matrix its own.
    void buildStillEntity(const World& world, Entity entity, const ModelRenderer& renderer, float alpha, std::span<DrawItem> draws) {
        buildEntityDraws(world, entity, renderer, alpha, nullptr, draws);

        for (DrawItem& draw : draws) {
            draw.previousModel = draw.model;
        }
    }

    constexpr std::size_t entitiesPerPiece = 256;
}

CellKey StillDraws::cellOfEntity(const World& world, Entity entity, float alpha) const {
    return worldCellOf(getRenderTransform(world, entity, alpha).position, cellSettings.size);
}

/*
* One walk over the renderers marks the still ones (drawn, placed, not deforming) and checks each against its entry:
* new ones (or another generation in a slot) join their cell, those with another number of parts rebuild theirs, kept
* ones not seen (destroyed, hidden, no longer still) leave theirs, and announced ones that changed cell leave one and
* join another; the cells touched are rebuilt, and announced changes within an untouched cell are built in place. The
* storages are looked up once, since a lookup by type per entity cost more than the rest of the walk.
*/
StillDraws::Update StillDraws::update(const World& world, float alpha, JobSystem& jobs) {
    Update result;
    result.previousVersion = version;
    result.version = ++version;
    const ComponentStorage<ModelRenderer>* renderers = world.storageOf<ModelRenderer>();

    if (renderers == nullptr) {
        valid = false;
        return result;
    }

    const std::vector<Entity>& entities = renderers->getEntities();
    const std::vector<ModelRenderer>& components = renderers->getComponents();
    const ComponentStorage<Transform>* transforms = world.storageOf<Transform>();
    const ComponentStorage<DeformingMesh>* deforming = world.storageOf<DeformingMesh>();
    const ComponentStorage<RenderChanged>* changes = world.storageOf<RenderChanged>();
    result.kept.assign(entities.size(), 0);

    const bool rebuild = !valid || !holding;
    std::vector<std::size_t> joining;
    std::vector<std::pair<CellKey, Entity>> leaving;
    std::vector<std::size_t> changed;
    std::set<CellKey> touched;
    std::size_t seenKept = 0;

    for (std::size_t index = 0; index < entities.size(); ++index) {
        const Entity entity = entities[index];
        const ModelRenderer& renderer = components[index];

        if (!renderer.still || !renderer.visible || transforms == nullptr || !transforms->contains(entity) || (deforming != nullptr && deforming->contains(entity))) {
            continue;
        }

        result.kept[index] = 1;

        if (rebuild) {
            continue;
        }

        Entry* entry = entity.index < byIndex.size() ? &byIndex[entity.index] : nullptr;

        // Another entity in a kept one's slot: the kept one leaves.
        if (entry != nullptr && entry->present && entry->generation != entity.generation) {
            leaving.emplace_back(entry->cell, Entity{entity.index, entry->generation});
            entry->present = false;
        }

        if (entry == nullptr || !entry->present) {
            joining.push_back(index);
            continue;
        }

        entry->seen = result.version;
        ++seenKept;

        if (entry->parts != renderer.parts.size()) {
            touched.insert(entry->cell);
        } else if (changes != nullptr && changes->contains(entity)) {
            changed.push_back(index);
        }
    }

    if (rebuild) {
        buildAll(world, result.kept, alpha, jobs);
        result.built.push_back(DrawRange{0, count});
    } else {
        // Last frame's moving and terrain draws after the still ones go first, so new slots are added after them.
        draws.resize(count);
        keptEntities -= leaving.size();

        // Kept entities not seen this frame: gone, hidden or no longer still. Only looked for when some are missing.
        if (seenKept < keptEntities) {
            for (const auto& [key, cell] : cells) {
                for (const Entity entity : cell.entities) {
                    Entry& entry = byIndex[entity.index];

                    if (entry.present && entry.generation == entity.generation && entry.seen != result.version) {
                        leaving.emplace_back(key, entity);
                        entry.present = false;
                        --keptEntities;
                    }
                }
            }
        }

        const auto leave = [&](const CellKey& key, Entity entity) {
            std::vector<Entity>& members = cells.at(key).entities;
            members.erase(std::find(members.begin(), members.end(), entity));
            touched.insert(key);
        };
        const auto join = [&](const CellKey& key, Entity entity) {
            cells[key].entities.push_back(entity);
            touched.insert(key);
        };

        for (const auto& [key, entity] : leaving) {
            leave(key, entity);
        }

        for (const std::size_t index : joining) {
            const Entity entity = entities[index];

            if (entity.index >= byIndex.size()) {
                byIndex.resize(static_cast<std::size_t>(entity.index) + 1);
            }

            const CellKey key = cellOfEntity(world, entity, alpha);
            byIndex[entity.index] = Entry{entity.generation, 0, static_cast<std::uint32_t>(components[index].parts.size()), true, key, result.version};
            join(key, entity);
            ++keptEntities;
        }

        std::vector<std::size_t> inPlace;

        for (const std::size_t index : changed) {
            Entry& entry = byIndex[entities[index].index];
            const CellKey key = cellOfEntity(world, entities[index], alpha);

            if (key != entry.cell) {
                leave(entry.cell, entities[index]);
                join(key, entities[index]);
                entry.cell = key;
            } else {
                inPlace.push_back(index);
            }
        }

        for (const CellKey& key : touched) {
            Cell& cell = cells.at(key);
            filled -= cell.used;
            cell.used = 0;
            std::size_t needed = 0;

            for (const Entity entity : cell.entities) {
                needed += world.tryGet<ModelRenderer>(entity)->parts.size();
            }

            // Left by every entity, or outgrown: its block emptied and given back (reported built, so what was there goes).
            if (cell.slots > 0 && (cell.entities.empty() || needed > cell.slots)) {
                emptySlots(cell.first, cell.slots);
                result.built.push_back(DrawRange{cell.first, cell.slots});
                blocks.giveBack(cell.first, cell.slots);
                cell.slots = 0;
            }

            if (cell.entities.empty()) {
                cells.erase(key);
                continue;
            }

            if (cell.slots == 0) {
                cell.slots = cellBlockSlots(needed, cellSettings);
                cell.first = blocks.take(cell.slots);

                if (blocks.end() > count) {
                    count = blocks.end();
                    draws.resize(count, emptyDrawSlot());
                }
            }

            buildCell(world, cell, alpha, jobs);
            cell.used = needed;
            filled += needed;
            result.built.push_back(DrawRange{cell.first, cell.slots});
        }

        for (const std::size_t index : inPlace) {
            const Entry& entry = byIndex[entities[index].index];

            if (touched.contains(entry.cell)) {
                continue;
            }

            buildStillEntity(world, entities[index], components[index], alpha, std::span<DrawItem>{draws}.subspan(entry.first, entry.parts));
            result.built.push_back(DrawRange{entry.first, entry.parts});
        }

        result.built = joinDrawRanges(std::move(result.built));
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

/*
* Every kept entity's cell found (on jobs), the entities grouped by cell in the renderers' order (a stable sort), each
* cell given a block in key order, then every entity built on jobs; the slots left empty.
*/
void StillDraws::buildAll(const World& world, const std::vector<std::uint8_t>& kept, float alpha, JobSystem& jobs) {
    const ComponentStorage<ModelRenderer>* renderers = world.storageOf<ModelRenderer>();
    const std::vector<Entity>& entities = renderers->getEntities();
    const std::vector<ModelRenderer>& components = renderers->getComponents();

    for (const auto& [key, cell] : cells) {
        for (const Entity entity : cell.entities) {
            byIndex[entity.index].present = false;
        }
    }

    cells.clear();
    blocks.clear();
    keptEntities = 0;
    filled = 0;

    std::vector<CellKey> keys(entities.size());
    jobs.parallelFor(entities.size(), entitiesPerPiece, [&](std::size_t index) {
        if (kept[index] != 0) {
            keys[index] = cellOfEntity(world, entities[index], alpha);
        }
    });

    std::vector<std::size_t> byCell;

    for (std::size_t index = 0; index < entities.size(); ++index) {
        if (kept[index] != 0) {
            byCell.push_back(index);
        }
    }

    std::stable_sort(byCell.begin(), byCell.end(), [&](std::size_t a, std::size_t b) { return keys[a] < keys[b]; });

    for (std::size_t start = 0; start < byCell.size();) {
        const CellKey key = keys[byCell[start]];
        std::size_t end = start;
        std::size_t needed = 0;

        while (end < byCell.size() && keys[byCell[end]] == key) {
            needed += components[byCell[end]].parts.size();
            ++end;
        }

        Cell& cell = cells[key];
        cell.used = needed;
        filled += needed;
        cell.slots = cellBlockSlots(needed, cellSettings);
        cell.first = blocks.take(cell.slots);
        std::size_t next = cell.first;

        for (std::size_t member = start; member < end; ++member) {
            const Entity entity = entities[byCell[member]];
            const std::size_t parts = components[byCell[member]].parts.size();

            if (entity.index >= byIndex.size()) {
                byIndex.resize(static_cast<std::size_t>(entity.index) + 1);
            }

            byIndex[entity.index] = Entry{entity.generation, static_cast<std::uint32_t>(next), static_cast<std::uint32_t>(parts), true, key, version};
            cell.entities.push_back(entity);
            next += parts;
            ++keptEntities;
        }

        start = end;
    }

    count = blocks.end();
    draws.clear();
    draws.resize(count, emptyDrawSlot());

    jobs.parallelFor(byCell.size(), entitiesPerPiece, [&](std::size_t member) {
        const std::size_t index = byCell[member];
        const Entry& entry = byIndex[entities[index].index];
        buildStillEntity(world, entities[index], components[index], alpha, std::span<DrawItem>{draws}.subspan(entry.first, entry.parts));
    });
}

// The cell's entities placed one after another from its first slot and built on jobs, its slots after them emptied.
void StillDraws::buildCell(const World& world, Cell& cell, float alpha, JobSystem& jobs) {
    std::size_t next = cell.first;

    for (const Entity entity : cell.entities) {
        Entry& entry = byIndex[entity.index];
        entry.first = static_cast<std::uint32_t>(next);
        entry.parts = static_cast<std::uint32_t>(world.tryGet<ModelRenderer>(entity)->parts.size());
        next += entry.parts;
    }

    emptySlots(next, cell.first + cell.slots - next);
    constexpr std::size_t cellEntitiesPerPiece = 64;
    jobs.parallelFor(cell.entities.size(), cellEntitiesPerPiece, [&](std::size_t member) {
        const Entity entity = cell.entities[member];
        const Entry& entry = byIndex[entity.index];
        buildStillEntity(world, entity, *world.tryGet<ModelRenderer>(entity), alpha, std::span<DrawItem>{draws}.subspan(entry.first, entry.parts));
    });
}

void StillDraws::emptySlots(std::size_t first, std::size_t slots) {
    std::fill(draws.begin() + static_cast<std::ptrdiff_t>(first), draws.begin() + static_cast<std::ptrdiff_t>(first + slots), emptyDrawSlot());
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

void StillDraws::setCells(const CellSettings& settings) {
    if (!(settings.size > 0.0f)) {
        throw std::invalid_argument("A cell's side must be above 0 m");
    }

    if (settings != cellSettings) {
        cellSettings = settings;
        valid = false;
    }
}

/*
* Each kept entity's matrices are worked out again as buildFrame works them out, so they match bit for bit unless the
* entity moved; one marked and not built yet is skipped, as is one gone (the next frame builds them all anyway). The
* entries are walked in turn, those not kept passed over.
*/
void StillDraws::verify(const World& world, std::size_t checks) {
    if (!holding || !valid || keptEntities == 0) {
        return;
    }

    for (std::size_t check = 0; check < checks && check < byIndex.size(); ++check) {
        verifyNext = (verifyNext + 1) % byIndex.size();
        const Entry& entry = byIndex[verifyNext];

        if (!entry.present) {
            continue;
        }

        const Entity entity{static_cast<std::uint32_t>(verifyNext), entry.generation};
        const ModelRenderer* renderer = world.isAlive(entity) ? world.tryGet<ModelRenderer>(entity) : nullptr;

        if (renderer == nullptr || !world.has<Transform>(entity) || world.has<RenderChanged>(entity)) {
            continue;
        }

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

std::size_t StillDraws::getCellCount() const {
    return cells.size();
}

std::size_t StillDraws::getFilledCount() const {
    return filled;
}
