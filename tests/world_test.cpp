#include "ecs/world.h"

#include <cassert>
#include <stdexcept>
#include <vector>

namespace {
    struct Position {
        float x = 0.0f;
    };

    struct Velocity {
        float x = 0.0f;
    };

    struct Charge {
        float value = 0.0f;
    };
}

void testWorld() {
    // A default Entity{} is never alive, so it can mean "no entity".
    {
        World world;
        assert(!world.isAlive(Entity{}));
        assert(world.getEntityCount() == 0);
    }

    // Creating, adding, reading and removing components.
    {
        World world;
        const Entity entity = world.create();

        assert(world.isAlive(entity));
        assert(world.getEntityCount() == 1);
        assert(!world.has<Position>(entity));
        assert(world.tryGet<Position>(entity) == nullptr);

        world.add(entity, Position{2.0f});
        assert(world.has<Position>(entity));
        assert(world.get<Position>(entity).x == 2.0f);

        // get() returns a reference into the storage, so changes stick.
        world.get<Position>(entity).x = 5.0f;
        assert(world.get<Position>(entity).x == 5.0f);

        world.remove<Position>(entity);
        assert(!world.has<Position>(entity));

        // Removing a component the entity does not have is harmless.
        world.remove<Velocity>(entity);
    }

    // An entity holds at most one component of each type, and dead entities take none.
    {
        World world;
        const Entity entity = world.create();
        world.add(entity, Position{});

        bool rejected = false;
        try {
            world.add(entity, Position{});
        } catch (const std::logic_error&) {
            rejected = true;
        }
        assert(rejected);

        world.destroy(entity);
        rejected = false;
        try {
            world.add(entity, Velocity{});
        } catch (const std::logic_error&) {
            rejected = true;
        }
        assert(rejected);
    }

    // Removing from the middle moves the last component into the hole without mixing up owners.
    {
        World world;
        const Entity a = world.create();
        const Entity b = world.create();
        const Entity c = world.create();

        world.add(a, Position{1.0f});
        world.add(b, Position{2.0f});
        world.add(c, Position{3.0f});

        world.remove<Position>(a);

        assert(!world.has<Position>(a));
        assert(world.get<Position>(b).x == 2.0f);
        assert(world.get<Position>(c).x == 3.0f);
    }

    // Destroying an entity removes its components, and its handle goes stale even after the slot is reused.
    {
        World world;
        const Entity old = world.create();
        world.add(old, Position{1.0f});

        world.destroy(old);
        assert(!world.isAlive(old));
        assert(world.getEntityCount() == 0);

        const Entity reused = world.create();
        assert(reused.index == old.index);
        assert(reused.generation != old.generation);
        assert(world.isAlive(reused));
        assert(!world.isAlive(old));

        // The new entity starts empty, and the old handle cannot reach its data.
        assert(!world.has<Position>(reused));
        world.add(reused, Position{9.0f});
        assert(!world.has<Position>(old));
        assert(world.tryGet<Position>(old) == nullptr);

        // Destroying a stale handle does nothing to the new entity.
        world.destroy(old);
        assert(world.isAlive(reused));
    }

    // each() visits exactly the entities that have all the listed components.
    {
        World world;
        const Entity moving = world.create();
        const Entity still = world.create();
        const Entity charged = world.create();

        world.add(moving, Position{0.0f});
        world.add(moving, Velocity{2.0f});
        world.add(still, Position{5.0f});
        world.add(charged, Position{1.0f});
        world.add(charged, Velocity{-1.0f});
        world.add(charged, Charge{3.0f});

        // A tiny integrator: position += velocity * dt for everything that has both.
        int visited = 0;
        world.each<Velocity, Position>([&](Entity, Velocity& velocity, Position& position) {
            position.x += velocity.x * 0.5f;
            ++visited;
        });

        assert(visited == 2);
        assert(world.get<Position>(moving).x == 1.0f);
        assert(world.get<Position>(still).x == 5.0f);
        assert(world.get<Position>(charged).x == 0.5f);

        // Three components at once.
        std::vector<Entity> matches;
        world.each<Charge, Position, Velocity>([&](Entity entity, Charge&, Position&, Velocity&) {
            matches.push_back(entity);
        });
        assert(matches.size() == 1);
        assert(matches[0] == charged);

        // A component type that was never added matches nothing.
        struct Unused {};
        visited = 0;
        world.each<Position, Unused>([&](Entity, Position&, Unused&) {
            ++visited;
        });
        assert(visited == 0);
    }

    // Read-only access through a const World.
    {
        World world;
        const Entity entity = world.create();
        world.add(entity, Position{4.0f});

        const World& readOnly = world;
        assert(readOnly.has<Position>(entity));
        assert(readOnly.get<Position>(entity).x == 4.0f);
        assert(readOnly.tryGet<Velocity>(entity) == nullptr);

        // Systems that only look (debug drawing, rendering) can walk a const World and get const components.
        world.add(world.create(), Position{6.0f});
        float sum = 0.0f;
        readOnly.each<Position>([&](Entity, const Position& position) {
            sum += position.x;
        });
        assert(sum == 10.0f);
    }
}
