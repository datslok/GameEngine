#include "math/transform.h"
#include "physics/collision_scene.h"
#include "physics/overlap.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.001f;
    }

    class Numbers {
    public:
        float next(float low, float high) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const float unit = static_cast<float>(state >> 40) / static_cast<float>(1ull << 24);
            return low + (high - low) * unit;
        }

        Vec3 nextPoint(float spread, float height) {
            return Vec3{next(-spread, spread), next(-height, height), next(-spread, spread)};
        }

    private:
        std::uint64_t state = 777;
    };

    Entity spawn(World& world, const Transform& transform, const Collider& collider) {
        const Entity entity = world.create();
        world.add(entity, transform);
        world.add(entity, collider);
        return entity;
    }

    // A world of mixed shapes, scattered, scaled and turned.
    void fillRandomWorld(World& world, Numbers& numbers, int count) {
        for (int i = 0; i < count; ++i) {
            Transform transform;
            transform.position = numbers.nextPoint(20.0f, 4.0f);
            transform.rotation = Vec3{0.0f, numbers.next(-3.0f, 3.0f), numbers.next(-0.5f, 0.5f)};
            const float size = numbers.next(0.3f, 1.5f);
            transform.scale = Vec3{size, size * numbers.next(0.5f, 2.0f), size};

            Shape shape;
            const int kind = i % 3;
            if (kind == 0) {
                shape = Sphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f};
            } else if (kind == 1) {
                shape = Capsule{Vec3{0.0f, -0.5f, 0.0f}, Vec3{0.0f, 0.5f, 0.0f}, 0.5f};
            } else {
                shape = Aabb{Vec3{-1.0f, -1.0f, -1.0f}, Vec3{1.0f, 1.0f, 1.0f}};
            }
            spawn(world, transform, Collider{shape, nullptr});
        }
    }

    // The answers without any broad phase: test every collider.
    std::optional<ColliderHit> bruteForceRaycast(const World& world, const Ray& ray) {
        std::optional<ColliderHit> best;
        world.each<Collider, Transform>([&](Entity entity, const Collider& collider, const Transform& transform) {
            const auto hit = raycast(ray, worldShape(collider.shape, transform.getMatrix()));
            if (hit && (!best || hit->distance < best->hit.distance)) {
                best = ColliderHit{entity, *hit};
            }
        });
        return best;
    }

    std::vector<Entity> bruteForceOverlapping(const World& world, const Shape& shape) {
        std::vector<Entity> found;
        world.each<Collider, Transform>([&](Entity entity, const Collider& collider, const Transform& transform) {
            if (overlaps(worldShape(collider.shape, transform.getMatrix()), shape)) {
                found.push_back(entity);
            }
        });
        return found;
    }

    bool sameEntities(std::vector<Entity> a, std::vector<Entity> b) {
        const auto order = [](Entity x, Entity y) { return x.index < y.index; };
        std::sort(a.begin(), a.end(), order);
        std::sort(b.begin(), b.end(), order);
        return a == b;
    }

    // The broad phase may only make queries faster, never change their answers.
    void testMatchesBruteForce() {
        Numbers numbers;
        for (int round = 0; round < 20; ++round) {
            World world;
            fillRandomWorld(world, numbers, 60);

            CollisionScene scene{CollisionSettings{.gridCellSize = numbers.next(0.5f, 4.0f)}};
            scene.rebuild(world);
            assert(scene.getColliders().size() == 60);

            for (int query = 0; query < 30; ++query) {
                const Ray ray{numbers.nextPoint(25.0f, 8.0f), numbers.nextPoint(1.0f, 1.0f)};
                const auto expected = bruteForceRaycast(world, ray);
                const auto actual = raycast(scene, ray, RaycastDetail::Shapes);
                assert(expected.has_value() == actual.has_value());
                if (expected) {
                    assert(nearlyEqual(actual->hit.distance, expected->hit.distance));
                }

                const Shape probe = Sphere{numbers.nextPoint(20.0f, 4.0f), numbers.next(0.2f, 3.0f)};
                assert(sameEntities(overlapping(scene, probe), bruteForceOverlapping(world, probe)));
            }
        }
    }

    // A row of 50 balls along x: a ray down the row meets the first one, and only a handful are tested.
    void testFewCollidersTested() {
        World world;
        Entity first;
        for (int i = 0; i < 50; ++i) {
            Transform transform;
            transform.position = Vec3{static_cast<float>(i) * 3.0f, 0.0f, 0.0f};
            const Entity entity = spawn(world, transform, Collider{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f}, nullptr});
            if (i == 0) {
                first = entity;
            }
        }

        CollisionScene scene;
        scene.rebuild(world);

        CollisionQueryStats stats;
        const auto hit = raycast(scene, Ray{Vec3{-10.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, RaycastDetail::Shapes,
                                 unlimitedRayDistance, &stats);
        assert(hit && hit->entity == first);
        assert(nearlyEqual(hit->hit.distance, 9.0f));
        assert(stats.tested <= 3);
        assert(stats.cellsWalked >= 1 && stats.cellsWalked <= 4);
    }

    // Detail meshes still confirm shape hits through the scene, and a rebuild forgets destroyed entities.
    void testDetailAndRebuild() {
        auto panel = std::make_shared<TriangleMesh>();
        panel->positions = {
            Vec3{-2.0f, -1.0f, 0.0f}, Vec3{-0.5f, -1.0f, 0.0f}, Vec3{-0.5f, 1.0f, 0.0f},
            Vec3{0.5f, -1.0f, 0.0f}, Vec3{2.0f, -1.0f, 0.0f}, Vec3{0.5f, 1.0f, 0.0f}
        };
        panel->indices = {0, 1, 2, 3, 4, 5};

        World world;
        const Entity front = spawn(world, Transform{}, meshCollider(panel));
        Transform back;
        back.position = Vec3{0.0f, 0.0f, -5.0f};
        const Entity behind = spawn(world, back, Collider{Sphere{Vec3{0.0f, 0.0f, 0.0f}, 1.0f}, nullptr});

        CollisionScene scene;
        scene.rebuild(world);

        // Through the gap between the panel's triangles: the panel's box is hit, its triangles are not.
        const Ray throughGap{Vec3{0.0f, 0.0f, 10.0f}, Vec3{0.0f, 0.0f, -1.0f}};
        assert(raycast(scene, throughGap, RaycastDetail::Shapes)->entity == front);
        assert(raycast(scene, throughGap, RaycastDetail::Triangles)->entity == behind);

        // Overlap queries use the shapes (a sphere in the gap touches the panel's box).
        assert(sameEntities(overlapping(scene, Sphere{Vec3{0.0f, 0.0f, 0.0f}, 0.1f}), std::vector<Entity>{front}));

        world.destroy(front);
        scene.rebuild(world);
        assert(scene.getColliders().size() == 1);
        assert(raycast(scene, throughGap, RaycastDetail::Shapes)->entity == behind);

        // The cell size is read at each rebuild, like every settings struct.
        scene.getSettings().gridCellSize = 5.0f;
        scene.rebuild(world);
        assert(scene.getGrid().getCellSize() == 5.0f);
    }
}

void testCollisionScene() {
    testMatchesBruteForce();
    testFewCollidersTested();
    testDetailAndRebuild();
}
