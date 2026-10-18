#include "physics/static_level.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <stdexcept>
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

        Vec3 nextPoint(float spread) {
            return Vec3{next(-spread, spread), next(-spread, spread), next(-spread, spread)};
        }

    private:
        std::uint64_t state = 99;
    };

    // A square floor of two triangles, size x size, at height y, front faces up.
    TriangleMesh floorSquare(float size, float y) {
        TriangleMesh mesh;
        const float half = size * 0.5f;
        mesh.positions = {Vec3{-half, y, -half}, Vec3{-half, y, half}, Vec3{half, y, half}, Vec3{half, y, -half}};
        mesh.indices = {0, 1, 2, 0, 2, 3};
        return mesh;
    }

    // Lots of small scattered triangles.
    TriangleMesh rubble(Numbers& numbers, int count) {
        TriangleMesh mesh;
        for (int i = 0; i < count; ++i) {
            const Vec3 centre = numbers.nextPoint(15.0f);
            for (int corner = 0; corner < 3; ++corner) {
                mesh.positions.push_back(centre + numbers.nextPoint(1.5f));
                mesh.indices.push_back(static_cast<std::uint32_t>(mesh.positions.size() - 1));
            }
        }
        return mesh;
    }

    std::optional<LevelHit> bruteForceRaycast(const StaticLevel& level, const Ray& ray) {
        std::optional<LevelHit> best;
        for (std::uint32_t triangle = 0; triangle < level.getTriangleCount(); ++triangle) {
            const auto corners = level.getTriangle(triangle);
            const auto hit = raycastTriangle(ray, corners[0], corners[1], corners[2]);
            if (hit && (!best || hit->distance < best->hit.distance)) {
                best = LevelHit{*hit, triangle};
            }
        }
        return best;
    }

    std::vector<std::uint32_t> bruteForceContacts(const StaticLevel& level, const Capsule& capsule) {
        std::vector<std::uint32_t> touched;
        for (std::uint32_t triangle = 0; triangle < level.getTriangleCount(); ++triangle) {
            const auto corners = level.getTriangle(triangle);
            if (contact(capsule, corners[0], corners[1], corners[2])) {
                touched.push_back(triangle);
            }
        }
        return touched;
    }

    std::vector<std::uint32_t> trianglesOf(const std::vector<Contact>& contacts) {
        std::vector<std::uint32_t> triangles;
        for (const Contact& found : contacts) {
            triangles.push_back(found.triangle);
        }
        std::sort(triangles.begin(), triangles.end());
        return triangles;
    }

    void testBuilding() {
        StaticLevel level;
        assert(level.getTriangleCount() == 0);

        // Added pieces are moved by their matrix and numbered after what is already there.
        level.add(floorSquare(2.0f, 0.0f));
        level.add(floorSquare(2.0f, 0.0f), Mat4::translation(10.0f, 3.0f, 0.0f));
        assert(level.getTriangleCount() == 4);
        const auto moved = level.getTriangle(2);
        assert(nearlyEqual(moved[0].x, 9.0f) && nearlyEqual(moved[0].y, 3.0f));

        // A piece whose triangle points at a missing corner is refused, and the level is left as it was.
        TriangleMesh broken = floorSquare(2.0f, 0.0f);
        broken.indices[5] = 7;
        bool threw = false;
        try {
            level.add(broken);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
        assert(level.getTriangleCount() == 4);
    }

    void testQueries() {
        StaticLevel level;
        level.add(floorSquare(40.0f, 0.0f)); // big enough for the grid's large list
        level.add(floorSquare(2.0f, 1.0f), Mat4::translation(5.0f, 0.0f, 0.0f)); // a small table top at height 1

        // Down onto the table, and down onto the floor beside it, where the huge floor triangles are found.
        const auto table = raycast(level, Ray{Vec3{5.0f, 10.0f, 0.0f}, Vec3{0.0f, -1.0f, 0.0f}});
        assert(table && nearlyEqual(table->hit.distance, 9.0f) && table->triangle >= 2);
        const auto floor = raycast(level, Ray{Vec3{-8.0f, 10.0f, 3.0f}, Vec3{0.0f, -1.0f, 0.0f}});
        assert(floor && nearlyEqual(floor->hit.distance, 10.0f) && floor->triangle < 2);
        assert(nearlyEqual(floor->hit.normal.y, 1.0f));

        // A capsule standing on the floor, sunk 0.05, touches the floor triangle under it and nothing else.
        const Capsule standing{Vec3{-8.0f, 0.45f, 3.0f}, Vec3{-8.0f, 1.45f, 3.0f}, 0.5f};
        const std::vector<Contact> touching = contacts(level, standing);
        assert(!touching.empty());
        for (const Contact& found : touching) {
            assert(found.triangle < 2);
            assert(nearlyEqual(found.depth, 0.05f));
            assert(nearlyEqual(found.normal.y, 1.0f));
        }

        // Floating in the air: no contacts.
        assert(contacts(level, Capsule{Vec3{-8.0f, 3.0f, 3.0f}, Vec3{-8.0f, 4.0f, 3.0f}, 0.5f}).empty());
    }

    // The grid may only make queries faster, never change their answers.
    void testMatchesBruteForce() {
        Numbers numbers;
        for (int round = 0; round < 10; ++round) {
            StaticLevel level{CollisionSettings{.gridCellSize = numbers.next(0.5f, 3.0f)}};
            level.add(rubble(numbers, 150));
            level.add(floorSquare(50.0f, -16.0f));

            for (int query = 0; query < 40; ++query) {
                const Ray ray{numbers.nextPoint(20.0f), numbers.nextPoint(1.0f)};
                const auto expected = bruteForceRaycast(level, ray);
                const auto actual = raycast(level, ray);
                assert(expected.has_value() == actual.has_value());
                if (expected) {
                    assert(nearlyEqual(actual->hit.distance, expected->hit.distance));
                }

                const Vec3 bottom = numbers.nextPoint(15.0f);
                const Capsule capsule{bottom, bottom + numbers.nextPoint(1.5f), numbers.next(0.2f, 1.5f)};
                assert(trianglesOf(contacts(level, capsule)) == bruteForceContacts(level, capsule));
            }
        }
    }

    // Through the grid, a ray tests only the triangles near its path.
    void testFewTrianglesTested() {
        Numbers numbers;
        StaticLevel level;
        level.add(rubble(numbers, 400));

        CollisionQueryStats stats;
        raycast(level, Ray{Vec3{-30.0f, 0.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}, unlimitedRayDistance, &stats);
        assert(stats.tested > 0);
        assert(stats.tested < 200);
        assert(stats.cellsWalked > 0);
    }
}

void testStaticLevel() {
    testBuilding();
    testQueries();
    testMatchesBruteForce();
    testFewTrianglesTested();
}
