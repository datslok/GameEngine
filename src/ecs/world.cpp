#include "ecs/world.h"

/*
* Reuse a freed slot when there is one, so entity indices stay small and the sparse arrays stay short.
*/
Entity World::create() {
    std::uint32_t index = 0;

    if (!freeIndices.empty()) {
        index = freeIndices.back();
        freeIndices.pop_back();
    } else {
        index = static_cast<std::uint32_t>(slots.size());
        slots.push_back(Slot{});
    }

    slots[index].alive = true;
    ++entityCount;

    return Entity{index, slots[index].generation};
}

void World::destroy(Entity entity) {
    if (activeWalks > 0) {
        defer([entity](World& world) { world.destroyNow(entity); });
        return;
    }

    destroyNow(entity);
}

/*
* Bumping the generation is what makes every existing handle to this entity stale. An entity already gone (destroyed
* twice, or by an earlier queued change) is left alone.
*/
void World::destroyNow(Entity entity) {
    if (!isAlive(entity)) {
        return;
    }

    for (auto& [type, storage] : storages) {
        storage->remove(entity);
    }

    Slot& slot = slots[entity.index];
    slot.alive = false;
    ++slot.generation;

    freeIndices.push_back(entity.index);
    --entityCount;
}

bool World::isAlive(Entity entity) const {
    return entity.index < slots.size() &&
        slots[entity.index].alive &&
        slots[entity.index].generation == entity.generation;
}

std::size_t World::getEntityCount() const {
    return entityCount;
}

/*
* The queue is taken out before running it, so a change that queues another (it cannot today, since no walk runs
* while it is applied, but a future one might walk) never invalidates the list being run.
*/
void World::applyPendingIfIdle() {
    while (activeWalks == 0 && !pending.empty()) {
        std::vector<std::unique_ptr<PendingChange>> changes = std::move(pending);
        pending.clear();

        for (const std::unique_ptr<PendingChange>& change : changes) {
            change->apply(*this);
        }
    }
}
