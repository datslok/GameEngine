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
    // Safe at any time, also inside each(): it touches no component array.
    Entity create();

    // Removes the entity and all its components. Handles to it become stale. Inside each() it is deferred until the
    // outermost walk ends (the entity and its components stay visible until then), so systems may destroy freely.
    void destroy(Entity entity);

    bool isAlive(Entity entity) const;
    std::size_t getEntityCount() const;

    /*
    * Attach a component. An entity can hold at most one component of each type.
    * Inside a walk over this component type it throws std::logic_error: the array could move under the walk, and a
    * deferred add could not hand back the component. Use addLater there. Other types can be added during a walk.
    */
    template <typename Component>
    Component& add(Entity entity, Component component) {
        if (!isAlive(entity)) {
            throw std::logic_error("Cannot add a component to a dead entity");
        }

        ComponentStorage<Component>& storage = getOrCreateStorage<Component>();

        if (storage.activeWalks > 0) {
            throw std::logic_error("Cannot add a component of a type being walked by each(); use addLater");
        }

        return storage.add(entity, std::move(component));
    }

    /*
    * Attach a component now, or, inside each(), when the outermost walk ends. A queued add to an entity destroyed
    * before it runs is skipped.
    */
    template <typename Component>
    void addLater(Entity entity, Component component) {
        if (activeWalks == 0) {
            add(entity, std::move(component));
            return;
        }

        defer([entity, component = std::move(component)](World& world) mutable {
            if (world.isAlive(entity)) {
                world.add(entity, std::move(component));
            }
        });
    }

    // Inside each() it is deferred until the outermost walk ends, like destroy.
    template <typename Component>
    void remove(Entity entity) {
        if (activeWalks > 0) {
            defer([entity](World& world) { world.remove<Component>(entity); });
            return;
        }

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
    * The function may change component values, create entities, destroy them and remove components (those two are
    * queued and done when the outermost walk ends, so the arrays being walked never change under it), and add
    * components of types not being walked; adding one of the walked types takes addLater.
    */
    template <typename First, typename... Rest, typename Function>
    void each(Function&& function) {
        ComponentStorage<First>* first = findStorage<First>();
        const std::tuple<ComponentStorage<Rest>*...> rest{findStorage<Rest>()...};

        // A type that was never added means no entity can match.
        if (!first || !(std::get<ComponentStorage<Rest>*>(rest) && ...)) {
            return;
        }

        const auto countWalk = [&](int change) {
            activeWalks += change;
            first->activeWalks += change;
            ((std::get<ComponentStorage<Rest>*>(rest)->activeWalks += change), ...);
        };

        countWalk(1);

        try {
            const std::vector<Entity>& entities = first->getEntities();
            std::vector<First>& components = first->getComponents();

            for (std::size_t i = 0; i < entities.size(); ++i) {
                const Entity entity = entities[i];

                if ((std::get<ComponentStorage<Rest>*>(rest)->contains(entity) && ...)) {
                    function(entity, components[i], *std::get<ComponentStorage<Rest>*>(rest)->tryGet(entity)...);
                }
            }
        }
        catch (...) {
            // A failing system still ends its walk, so the world does not stay locked; what it queued is applied.
            countWalk(-1);
            applyPendingIfIdle();
            throw;
        }

        countWalk(-1);
        applyPendingIfIdle();
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

    // A change asked for during a walk, waiting for the outermost walk to end. Type-erased, because removes and adds
    // name their component type.
    struct PendingChange {
        virtual ~PendingChange() = default;
        virtual void apply(World& world) = 0;
    };

    template <typename Call>
    struct PendingCall final : PendingChange {
        explicit PendingCall(Call call): call(std::move(call)) {}
        void apply(World& world) override { call(world); }
        Call call;
    };

    template <typename Call>
    void defer(Call call) {
        pending.push_back(std::make_unique<PendingCall<Call>>(std::move(call)));
    }

    // Runs the queued changes in the order they were asked for, once no walk is running.
    void applyPendingIfIdle();

    void destroyNow(Entity entity);

    // How many walks are running (they can nest), and the changes waiting for them to end.
    int activeWalks = 0;
    std::vector<std::unique_ptr<PendingChange>> pending;

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
