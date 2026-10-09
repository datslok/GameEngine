#pragma once

#include "ecs/component_storage.h"
#include "ecs/entity.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

/*
* Owns every entity and component. Game code creates entities, attaches components to them, and runs systems over them with each().
* Think of it as a table: one row per entity, one column per component type, with gaps where an entity lacks a component.
*/
class World {
public:
    Entity create();

    // Removes the entity and all its components. Handles to it become stale.
    void destroy(Entity entity);

    bool isAlive(Entity entity) const;
    std::size_t getEntityCount() const;

    /*
    * Attach a component. An entity can hold at most one component of each type.
    */
    template <typename Component>
    Component& add(Entity entity, Component component) {
        if (!isAlive(entity)) {
            throw std::logic_error("Cannot add a component to a dead entity");
        }

        return getOrCreateStorage<Component>().add(entity, std::move(component));
    }

    template <typename Component>
    void remove(Entity entity) {
        if (ComponentStorage<Component>* storage = findStorage<Component>()) {
            storage->remove(entity);
        }
    }

    template <typename Component>
    bool has(Entity entity) const {
        const ComponentStorage<Component>* storage = findStorage<Component>();
        return storage && storage->contains(entity);
    }

    // Returns nullptr when the entity does not have the component.
    template <typename Component>
    Component* tryGet(Entity entity) {
        ComponentStorage<Component>* storage = findStorage<Component>();
        return storage ? storage->tryGet(entity) : nullptr;
    }

    template <typename Component>
    const Component* tryGet(Entity entity) const {
        const ComponentStorage<Component>* storage = findStorage<Component>();
        return storage ? storage->tryGet(entity) : nullptr;
    }

    // Throws when the entity does not have the component, for code that requires it.
    template <typename Component>
    Component& get(Entity entity) {
        Component* component = tryGet<Component>(entity);
        if (!component) {
            throw std::logic_error("Entity does not have the requested component");
        }
        return *component;
    }

    template <typename Component>
    const Component& get(Entity entity) const {
        const Component* component = tryGet<Component>(entity);
        if (!component) {
            throw std::logic_error("Entity does not have the requested component");
        }
        return *component;
    }

    /*
    * Run a system: call function(entity, first, rest...) for every entity that has all the listed components.
    * Example: world.each<Transform, Spinner>([](Entity entity, Transform& transform, Spinner& spinner) { ... });
    * It walks the packed array of the first type and skips entities missing any of the others, so list the rarest component first.
    * The function may change component values, but must not add or remove components of the listed types or destroy entities, because that reorders the arrays being walked.
    */
    template <typename First, typename... Rest, typename Function>
    void each(Function&& function) {
        ComponentStorage<First>* first = findStorage<First>();
        const std::tuple<ComponentStorage<Rest>*...> rest{findStorage<Rest>()...};

        // A type that was never added means no entity can match.
        if (!first || !(std::get<ComponentStorage<Rest>*>(rest) && ...)) {
            return;
        }

        const std::vector<Entity>& entities = first->getEntities();
        std::vector<First>& components = first->getComponents();

        for (std::size_t i = 0; i < entities.size(); ++i) {
            const Entity entity = entities[i];

            if ((std::get<ComponentStorage<Rest>*>(rest)->contains(entity) && ...)) {
                function(entity, components[i], *std::get<ComponentStorage<Rest>*>(rest)->tryGet(entity)...);
            }
        }
    }

    /*
    * The read-only version, for code that only looks: the function gets const components.
    * It reuses the walk above; casting away const is safe because nothing is changed through it.
    */
    template <typename First, typename... Rest, typename Function>
    void each(Function&& function) const {
        const_cast<World*>(this)->each<First, Rest...>([&](Entity entity, const First& first, const Rest&... rest) {
            function(entity, first, rest...);
        });
    }

private:
    struct Slot {
        std::uint32_t generation = 1;
        bool alive = false;
    };

    template <typename Component>
    ComponentStorage<Component>* findStorage() {
        const auto found = storages.find(std::type_index(typeid(Component)));
        if (found == storages.end()) {
            return nullptr;
        }
        return static_cast<ComponentStorage<Component>*>(found->second.get());
    }

    template <typename Component>
    const ComponentStorage<Component>* findStorage() const {
        const auto found = storages.find(std::type_index(typeid(Component)));
        if (found == storages.end()) {
            return nullptr;
        }
        return static_cast<const ComponentStorage<Component>*>(found->second.get());
    }

    template <typename Component>
    ComponentStorage<Component>& getOrCreateStorage() {
        if (ComponentStorage<Component>* storage = findStorage<Component>()) {
            return *storage;
        }

        auto storage = std::make_unique<ComponentStorage<Component>>();
        ComponentStorage<Component>& result = *storage;
        storages.emplace(std::type_index(typeid(Component)), std::move(storage));
        return result;
    }

    std::vector<Slot> slots;
    std::vector<std::uint32_t> freeIndices;
    std::size_t entityCount = 0;

    // One storage per component type, created the first time that type is added.
    std::unordered_map<std::type_index, std::unique_ptr<ComponentStorageBase>> storages;
};
