#pragma once

#include "ecs/entity.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

/*
* Lets the World remove a destroyed entity from every storage without knowing the component types.
*/
class ComponentStorageBase {
public:
    virtual ~ComponentStorageBase() = default;
    virtual void remove(Entity entity) = 0;
};

/*
* Stores one component type as a "sparse set".
* components holds the data packed back to back, so systems loop over a tight array.
* entities[i] says which entity owns components[i].
* sparse maps an entity index to its position in the packed arrays, so lookups are a single array access.
*/
template <typename Component>
class ComponentStorage final : public ComponentStorageBase {
public:
    /*
    * Append the component to the end of the packed arrays and remember where it went.
    */
    Component& add(Entity entity, Component component) {
        if (contains(entity)) {
            throw std::logic_error("Entity already has this component");
        }

        if (entity.index >= sparse.size()) {
            sparse.resize(static_cast<std::size_t>(entity.index) + 1, missing);
        }

        sparse[entity.index] = static_cast<std::uint32_t>(components.size());
        entities.push_back(entity);
        components.push_back(std::move(component));

        return components.back();
    }

    /*
    * Comparing the stored entity, not only the index, rejects stale handles whose slot has been reused.
    */
    bool contains(Entity entity) const {
        return entity.index < sparse.size() &&
            sparse[entity.index] != missing &&
            entities[sparse[entity.index]] == entity;
    }

    Component* tryGet(Entity entity) {
        return contains(entity) ? &components[sparse[entity.index]] : nullptr;
    }

    const Component* tryGet(Entity entity) const {
        return contains(entity) ? &components[sparse[entity.index]] : nullptr;
    }

    /*
    * Move the last component into the hole, so the arrays stay packed without shifting every element.
    * This changes the order of components, which is why systems must not rely on it.
    */
    void remove(Entity entity) override {
        if (!contains(entity)) {
            return;
        }

        const std::uint32_t hole = sparse[entity.index];
        const std::uint32_t last = static_cast<std::uint32_t>(components.size() - 1);

        if (hole != last) {
            components[hole] = std::move(components[last]);
            entities[hole] = entities[last];
            sparse[entities[hole].index] = hole;
        }

        components.pop_back();
        entities.pop_back();
        sparse[entity.index] = missing;
    }

    std::size_t size() const {
        return components.size();
    }

    const std::vector<Entity>& getEntities() const {
        return entities;
    }

    std::vector<Component>& getComponents() {
        return components;
    }

    const std::vector<Component>& getComponents() const {
        return components;
    }

private:
    static constexpr std::uint32_t missing = std::numeric_limits<std::uint32_t>::max();

    std::vector<std::uint32_t> sparse;
    std::vector<Entity> entities;
    std::vector<Component> components;
};
