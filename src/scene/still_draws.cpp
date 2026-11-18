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
* Not drawn without a renderer shown and a Transform; kept when still and not deforming; else built every frame.
*/
StillDraws::Kind StillDraws::kindOf(const World& world, Entity entity) {
    const ModelRenderer* renderer = world.isAlive(entity) ? world.tryGet<ModelRenderer>(entity) : nullptr;

    if (renderer == nullptr || !renderer->isVisible() || !world.has<Transform>(entity)) {
        return Kind::None;
    }

    return renderer->isStill() && !world.has<DeformingMesh>(entity) ? Kind::Kept : Kind::Moving;
}

/*
* The entities looked at are those the world's logs name (a renderer, Transform or deforming mesh gained or lost) and
* those announced (RenderChanged), each once, in entity order. Each one's kind now against its entry: kept ones that
* stop being kept leave their cell, new kept ones join theirs, kept ones with another number of parts rebuild theirs,
* and announced ones that changed cell leave one and join another; the cells touched are rebuilt, and announced changes
* within an untouched cell are built in place. Moving ones join or leave the moving list. A slot whose entity was
* destroyed (its index perhaps reused) is let go first. Everything is laid out afresh when the logs were off (the first
* update, or the kept draws turned off), when the list was not given back, or after invalidate.
*/
StillDraws::Update StillDraws::update(World& world, float alpha, JobSystem& jobs) {
    Update result;
    result.previousVersion = version;
    result.version = ++version;
    const bool logged = world.isLoggingChanges<ModelRenderer>() && world.isLoggingChanges<Transform>() && world.isLoggingChanges<DeformingMesh>();

    if (!valid || !holding || !logged) {
        layOutAfresh(world, alpha, jobs);
        result.built.push_back(DrawRange{0, count});
    } else {
        std::vector<Entity> looked = world.takeChanges<ModelRenderer>();
        const std::vector<Entity> transformChanges = world.takeChanges<Transform>();
        const std::vector<Entity> deformingChanges = world.takeChanges<DeformingMesh>();
        looked.insert(looked.end(), transformChanges.begin(), transformChanges.end());
        looked.insert(looked.end(), deformingChanges.begin(), deformingChanges.end());

        if (const ComponentStorage<RenderChanged>* changes = world.storageOf<RenderChanged>()) {
            looked.insert(looked.end(), changes->getEntities().begin(), changes->getEntities().end());
        }

        std::sort(looked.begin(), looked.end(), [](Entity a, Entity b) {
            return a.index != b.index ? a.index < b.index : a.generation < b.generation;
        });
        looked.erase(std::unique(looked.begin(), looked.end()), looked.end());

        // Last frame's moving and terrain draws after the still ones go first, so new slots are added after them.
        draws.resize(count);
        std::set<CellKey> touched;
        std::vector<Entity> inPlace;

        const auto leave = [&](const CellKey& key, Entity entity) {
            std::vector<Entity>& members = cells.at(key).entities;
            members.erase(std::find(members.begin(), members.end(), entity));
            touched.insert(key);
        };
        const auto join = [&](const CellKey& key, Entity entity) {
            cells[key].entities.push_back(entity);
            touched.insert(key);
        };
        // An entry stops being drawn: out of its cell or the moving list.
        const auto letGo = [&](Entry& entry, Entity entity) {
            if (entry.kind == Kind::Kept) {
                leave(entry.cell, entity);
                --keptEntities;
            } else if (entry.kind == Kind::Moving) {
                removeMoving(entry);
            }

            entry.kind = Kind::None;
        };

        for (const Entity entity : looked) {
            if (entity.index >= byIndex.size()) {
                byIndex.resize(static_cast<std::size_t>(entity.index) + 1);
            }

            Entry& entry = byIndex[entity.index];

            if (entry.generation != entity.generation) {
                const Entity held{entity.index, entry.generation};

                // The slot's entity was destroyed (and its index perhaps reused): it goes.
                if (entry.kind != Kind::None && !world.isAlive(held)) {
                    letGo(entry, held);
                }

                // A stale handle, or the slot held by a live entity: nothing of its own to change.
                if (entry.kind != Kind::None || !world.isAlive(entity)) {
                    continue;
                }

                entry.generation = entity.generation;
            }

            const Kind now = kindOf(world, entity);

            if (entry.kind == Kind::Kept && now == Kind::Kept) {
                const CellKey key = cellOfEntity(world, entity, alpha);

                if (key != entry.cell) {
                    leave(entry.cell, entity);
                    join(key, entity);
                    entry.cell = key;
                } else if (entry.parts != world.get<ModelRenderer>(entity).parts.size()) {
                    touched.insert(entry.cell);
                } else {
                    inPlace.push_back(entity);
                }

                continue;
            }

            if (entry.kind == now) {
                continue;
            }

            letGo(entry, entity);

            if (now == Kind::Kept) {
                entry.cell = cellOfEntity(world, entity, alpha);
                entry.parts = static_cast<std::uint32_t>(world.get<ModelRenderer>(entity).parts.size());
                join(entry.cell, entity);
                ++keptEntities;
            } else if (now == Kind::Moving) {
                addMoving(entity, entry);
            }

            entry.kind = now;
        }

        for (const CellKey& key : touched) {
            Cell& cell = cells.at(key);
            filled -= cell.used;
            cell.used = 0;
            std::size_t needed = 0;

            for (const Entity entity : cell.entities) {
                needed += world.get<ModelRenderer>(entity).parts.size();
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

            buildCell(world, key, cell, alpha, jobs);
            cell.used = needed;
            filled += needed;
            result.built.push_back(DrawRange{cell.first, cell.slots});
        }

        for (const Entity entity : inPlace) {
            const Entry& entry = byIndex[entity.index];

            if (touched.contains(entry.cell)) {
                continue;
            }

            buildStillEntity(world, entity, world.get<ModelRenderer>(entity), alpha, std::span<DrawItem>{draws}.subspan(entry.first, entry.parts));
            result.built.push_back(DrawRange{entry.first, entry.parts});
        }

        result.built = joinDrawRanges(std::move(result.built));
    }

    // Last frame's moving and terrain draws after them go.
    draws.resize(count);
    result.draws = std::move(draws);
    draws = {};
    result.count = count;
    result.moving = movingPlaces(world);
    holding = false;
    valid = true;
    return result;
}

/*
* The logs turned on and emptied (the walk covers what they held), one walk over every renderer sorting each into kept,
* moving or neither, and the kept ones laid out (buildAll).
*/
void StillDraws::layOutAfresh(World& world, float alpha, JobSystem& jobs) {
    world.logChanges<ModelRenderer>(true);
    world.logChanges<Transform>(true);
    world.logChanges<DeformingMesh>(true);
    world.takeChanges<ModelRenderer>();
    world.takeChanges<Transform>();
    world.takeChanges<DeformingMesh>();

    byIndex.clear();
    moving.clear();
    const std::vector<Entity>& entities = world.storageOf<ModelRenderer>()->getEntities();
    std::vector<std::uint8_t> kept(entities.size(), 0);
    std::size_t slots = 0;

    for (const Entity entity : entities) {
        slots = std::max<std::size_t>(slots, static_cast<std::size_t>(entity.index) + 1);
    }

    byIndex.resize(slots);

    for (std::size_t index = 0; index < entities.size(); ++index) {
        Entry& entry = byIndex[entities[index].index];
        entry.generation = entities[index].generation;
        entry.kind = kindOf(world, entities[index]);

        if (entry.kind == Kind::Kept) {
            kept[index] = 1;
        } else if (entry.kind == Kind::Moving) {
            addMoving(entities[index], entry);
        }
    }

    buildAll(world, kept, alpha, jobs);
}

void StillDraws::addMoving(Entity entity, Entry& entry) {
    entry.movingAt = static_cast<std::uint32_t>(moving.size());
    moving.push_back(entity);
}

// The last moving entity takes the hole.
void StillDraws::removeMoving(Entry& entry) {
    const Entity last = moving.back();
    moving[entry.movingAt] = last;
    byIndex[last.index].movingAt = entry.movingAt;
    moving.pop_back();
}

/*
* Sorted by their place in the renderers' storage, so the moving draws come in the order a walk over the world gives
* (the order buildFrame always gave them): a sort of the moving entities alone each frame, never a walk over them all.
*/
std::vector<std::size_t> StillDraws::movingPlaces(const World& world) const {
    std::vector<std::size_t> places;
    places.reserve(moving.size());
    const ComponentStorage<ModelRenderer>& renderers = *world.storageOf<ModelRenderer>();

    for (const Entity entity : moving) {
        places.push_back(renderers.placeOf(entity));
    }

    std::sort(places.begin(), places.end());
    return places;
}

void StillDraws::stopLogging(World& world) {
    world.logChanges<ModelRenderer>(false);
    world.logChanges<Transform>(false);
    world.logChanges<DeformingMesh>(false);
    valid = false;
}

/*
* Every kept entity's cell found (on jobs), the entities grouped by cell in the renderers' order (a stable sort), each
* cell given a block in key order, then every entity built on jobs; the slots left empty.
*/
void StillDraws::buildAll(const World& world, const std::vector<std::uint8_t>& kept, float alpha, JobSystem& jobs) {
    const ComponentStorage<ModelRenderer>* renderers = world.storageOf<ModelRenderer>();
    const std::vector<Entity>& entities = renderers->getEntities();
    const std::vector<ModelRenderer>& components = renderers->getComponents();
    cells.clear();
    blocks.clear();
    keptEntities = 0;
    filled = 0;

    std::vector<CellKey> keys(entities.size());
    std::vector<std::uint32_t> codes(entities.size(), 0);
    jobs.parallelFor(entities.size(), entitiesPerPiece, [&](std::size_t index) {
        if (kept[index] != 0) {
            const Vec3 position = getRenderTransform(world, entities[index], alpha).position;
            keys[index] = worldCellOf(position, cellSettings.size);
            codes[index] = cellMortonCode(position, keys[index], cellSettings.size);
        }
    });

    std::vector<std::size_t> byCell;

    for (std::size_t index = 0; index < entities.size(); ++index) {
        if (kept[index] != 0) {
            byCell.push_back(index);
        }
    }

    std::stable_sort(byCell.begin(), byCell.end(), [&](std::size_t a, std::size_t b) {
        return keys[a] != keys[b] ? keys[a] < keys[b] : codes[a] < codes[b];
    });

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

            byIndex[entity.index] = Entry{entity.generation, static_cast<std::uint32_t>(next), static_cast<std::uint32_t>(parts), Kind::Kept, key, 0};
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

/*
* The cell's entities put in Morton order (stable, so equal codes keep their order), placed one after another from its
* first slot and built on jobs, its slots after them emptied.
*/
void StillDraws::buildCell(const World& world, const CellKey& key, Cell& cell, float alpha, JobSystem& jobs) {
    std::vector<std::pair<std::uint32_t, Entity>> coded;
    coded.reserve(cell.entities.size());

    for (const Entity entity : cell.entities) {
        coded.emplace_back(cellMortonCode(getRenderTransform(world, entity, alpha).position, key, cellSettings.size), entity);
    }

    std::stable_sort(coded.begin(), coded.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (std::size_t member = 0; member < coded.size(); ++member) {
        cell.entities[member] = coded[member].second;
    }

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
* entity moved; its parts are counted and it must still be shown and still. One marked and not built yet is skipped, as
* is one whose components changed (the next update looks at it). The entries are walked in turn, those not kept passed
* over.
*/
void StillDraws::verify(const World& world, std::size_t checks) {
    if (!holding || !valid || keptEntities == 0) {
        return;
    }

    for (std::size_t check = 0; check < checks && check < byIndex.size(); ++check) {
        verifyNext = (verifyNext + 1) % byIndex.size();
        const Entry& entry = byIndex[verifyNext];

        if (entry.kind != Kind::Kept) {
            continue;
        }

        const Entity entity{static_cast<std::uint32_t>(verifyNext), entry.generation};
        const ModelRenderer* renderer = world.isAlive(entity) ? world.tryGet<ModelRenderer>(entity) : nullptr;

        if (renderer == nullptr || !world.has<Transform>(entity) || world.has<DeformingMesh>(entity) || world.has<RenderChanged>(entity)) {
            continue;
        }

        const std::string named = ": entity " + std::to_string(entity.index);

        if (!renderer->isStill() || !renderer->isVisible()) {
            throw std::logic_error("A still entity's renderer was replaced, no longer still or shown, without setRenderStill or setRenderVisible" + named);
        }

        if (entry.parts != renderer->parts.size()) {
            throw std::logic_error("A still entity (ModelRenderer::isStill) was given more or fewer parts without markRenderChanged" + named);
        }

        const Mat4 entityMatrix = getRenderTransform(world, entity, 1.0f).getMatrix();

        for (std::size_t part = 0; part < renderer->parts.size(); ++part) {
            if (!(entityMatrix * renderer->parts[part].localTransform == draws[entry.first + part].model)) {
                throw std::logic_error("A still entity (ModelRenderer::isStill) moved without markRenderChanged" + named);
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

std::size_t StillDraws::getChunkSlots() const {
    return static_cast<std::size_t>(std::max(cellSettings.chunkSlots, 1));
}
