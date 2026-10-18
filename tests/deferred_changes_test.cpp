#include "ecs/world.h"

#include <cassert>
#include <stdexcept>
#include <vector>

namespace {
    struct Health {
        int value = 0;
    };

    struct Poisoned {
        int turnsLeft = 0;
    };

    World worldWithHealth(int count, std::vector<Entity>& entities) {
        World world;

        for (int i = 0; i < count; ++i) {
            const Entity entity = world.create();
            world.add(entity, Health{i});
            entities.push_back(entity);
        }

        return world;
    }

    /*
    * Destroying entities while walking them is deferred to the end of the walk: every entity is still visited exactly
    * once (none skipped by a component moved into a visited slot), and the destroyed ones are gone afterwards.
    */
    void testDestroyDuringWalkIsDeferred() {
        std::vector<Entity> entities;
        World world = worldWithHealth(5, entities);
        int visits = 0;

        world.each<Health>([&](Entity entity, Health& health) {
            ++visits;

            if (health.value % 2 == 0) {
                world.destroy(entity);

                // Still there until the walk ends.
                assert(world.isAlive(entity));
                assert(world.has<Health>(entity));
            }
        });

        assert(visits == 5);
        assert(world.getEntityCount() == 2);
        assert(!world.isAlive(entities[0]) && world.isAlive(entities[1]) && !world.isAlive(entities[2]));
        assert(world.isAlive(entities[3]) && !world.isAlive(entities[4]));
    }

    // Removing a component of the type being walked is deferred the same way.
    void testRemoveDuringWalkIsDeferred() {
        std::vector<Entity> entities;
        World world = worldWithHealth(4, entities);
        int visits = 0;

        world.each<Health>([&](Entity entity, Health&) {
            ++visits;
            world.remove<Health>(entity);
        });

        assert(visits == 4);

        for (const Entity entity : entities) {
            assert(world.isAlive(entity) && !world.has<Health>(entity));
        }
    }

    // Walks can nest; changes wait for the outermost one to finish.
    void testNestedWalksWaitForTheOutermost() {
        std::vector<Entity> entities;
        World world = worldWithHealth(3, entities);
        world.add(entities[0], Poisoned{2});

        world.each<Health>([&](Entity, Health&) {
            world.each<Poisoned>([&](Entity poisoned, Poisoned&) {
                world.destroy(poisoned);
            });

            assert(world.isAlive(entities[0]));
        });

        assert(!world.isAlive(entities[0]));
        assert(world.getEntityCount() == 2);
    }

    /*
    * Adding a component of a type being walked could move the array under the walk, and a deferred add cannot hand
    * back a reference, so add() refuses and points to addLater(). Other types can be added at once, and entities can
    * be created at once (neither touches the arrays being walked).
    */
    void testAddDuringWalk() {
        std::vector<Entity> entities;
        World world = worldWithHealth(2, entities);
        bool threw = false;
        Entity spawned;

        world.each<Health>([&](Entity entity, Health&) {
            if (entity == entities[0]) {
                spawned = world.create();
                assert(world.isAlive(spawned));

                try {
                    world.add(spawned, Health{10});
                }
                catch (const std::logic_error&) {
                    threw = true;
                }

                Poisoned& poison = world.add(entity, Poisoned{3});
                assert(poison.turnsLeft == 3);

                world.addLater(spawned, Health{20});
                assert(!world.has<Health>(spawned));
            }
        });

        assert(threw);
        assert(world.has<Poisoned>(entities[0]));
        assert(world.get<Health>(spawned).value == 20);

        // Outside a walk, addLater adds straight away.
        const Entity late = world.create();
        world.addLater(late, Health{5});
        assert(world.get<Health>(late).value == 5);
    }

    /*
    * Queued changes run in the order they were asked for, and ones that no longer make sense (adding to an entity
    * destroyed earlier in the queue, destroying one twice) are skipped without error.
    */
    void testQueuedChangesKeepTheirOrder() {
        std::vector<Entity> entities;
        World world = worldWithHealth(2, entities);

        world.each<Health>([&](Entity entity, Health&) {
            if (entity == entities[0]) {
                world.addLater(entity, Poisoned{1});
                world.remove<Poisoned>(entity);
            } else {
                world.destroy(entity);
                world.destroy(entity);
                world.addLater(entity, Poisoned{4});
            }
        });

        assert(world.isAlive(entities[0]) && !world.has<Poisoned>(entities[0]));
        assert(!world.isAlive(entities[1]));
        assert(world.getEntityCount() == 1);
    }

    // A walk that throws still ends: changes asked for before the throw are applied, and the world accepts adds again.
    void testWalkThatThrowsStillEnds() {
        std::vector<Entity> entities;
        World world = worldWithHealth(2, entities);

        try {
            world.each<Health>([&](Entity entity, Health&) {
                world.destroy(entity);
                throw std::runtime_error("system failed");
            });
        }
        catch (const std::runtime_error&) {
        }

        assert(!world.isAlive(entities[0]) || !world.isAlive(entities[1]));
        assert(world.getEntityCount() == 1);

        const Entity added = world.create();
        world.add(added, Health{1});
        assert(world.has<Health>(added));
    }

    // A read-only walk is a walk too: it shares the walk counting with the writable one.
    void testConstWalkCounts() {
        std::vector<Entity> entities;
        World world = worldWithHealth(2, entities);
        const World& view = world;
        bool threw = false;

        view.each<Health>([&](Entity, const Health&) {
            try {
                world.add(world.create(), Health{3});
            }
            catch (const std::logic_error&) {
                threw = true;
            }
        });

        assert(threw);
    }
}

void testDeferredChanges() {
    testDestroyDuringWalkIsDeferred();
    testRemoveDuringWalkIsDeferred();
    testNestedWalksWaitForTheOutermost();
    testAddDuringWalk();
    testQueuedChangesKeepTheirOrder();
    testWalkThatThrowsStillEnds();
    testConstWalkCounts();
}
