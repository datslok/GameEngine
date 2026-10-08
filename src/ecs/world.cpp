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

/*
* Bumping the generation is what makes every existing handle to this entity stale.
*/
void World::destroy(Entity entity) {
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
