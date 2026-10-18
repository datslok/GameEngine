#include "physics/static_level.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <optional>

namespace {
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
        std::uint64_t state = 7;
    };

    bool nearVector(const Vec3& actual, const Vec3& expected, float tolerance) {
        return (actual - expected).length() <= tolerance;
    }

    // Lots of small scattered triangles.
    TriangleMesh rubble(Numbers& numbers, int count) {
        TriangleMesh mesh;
        for (int i = 0; i < count; ++i) {
            const Vec3 centre = numbers.nextPoint(6.0f);
            for (int corner = 0; corner < 3; ++corner) {
                mesh.positions.push_back(centre + numbers.nextPoint(1.0f));
                mesh.indices.push_back(static_cast<std::uint32_t>(mesh.positions.size() - 1));
            }
        }
        return mesh;
    }

    // Whether a sphere there presses into the level more than the given depth.
    bool overlapsLevel(const StaticLevel& level, const Sphere& sphere, float depth) {
        for (const Contact& found : contacts(level, Capsule{sphere.centre, sphere.centre, sphere.radius})) {
            if (found.depth > depth) {
                return true;
            }
        }
        return false;
    }

    // Falling onto a floor face: it stops where the sphere's bottom meets the floor, pushed straight back up.
    void testFace() {
        const StaticLevel level = flatLevel();
        const Sphere ball{Vec3{1.0f, 2.0f, -3.0f}, 0.5f};

        const auto hit = sweepSphere(level, ball, Vec3{0.0f, -3.0f, 0.0f});
        assert(hit);
        assert(near(hit->hit.distance, 1.5f, 1e-4f));
        assert(nearVector(hit->hit.point, Vec3{1.0f, 0.5f, -3.0f}, 1e-4f));
        assert(nearVector(hit->hit.normal, Vec3{0.0f, 1.0f, 0.0f}, 1e-4f));
        const auto floorTop = level.getTriangle(hit->triangle);
        assert(near(floorTop[0].y, 0.0f, 1e-5f) && near(floorTop[1].y, 0.0f, 1e-5f) && near(floorTop[2].y, 0.0f, 1e-5f));

        // Not far enough to reach it.
        assert(!sweepSphere(level, ball, Vec3{0.0f, -1.4f, 0.0f}));
        // Slanting down onto it: the distance is along the slanted path.
        const auto slanted = sweepSphere(level, ball, Vec3{3.0f, -3.0f, 0.0f});
        assert(slanted && near(slanted->hit.distance, 1.5f * std::sqrt(2.0f), 1e-4f));
        assert(nearVector(slanted->hit.normal, Vec3{0.0f, 1.0f, 0.0f}, 1e-4f));
    }

    // Past the top edge of a box: the sphere touches the edge, not the face, and is pushed away from the edge line.
    void testEdgeAndCorner() {
        StaticLevel level;
        level.add(block(Vec3{0.0f, 0.0f, -5.0f}, Vec3{10.0f, 1.0f, 5.0f}));

        // Centre 0.3 above the top edge (x = 0, y = 1): it touches when 0.4 short of the edge, since 0.4^2 + 0.3^2 = 0.5^2.
        const auto edge = sweepSphere(level, Sphere{Vec3{-2.0f, 1.3f, 0.0f}, 0.5f}, Vec3{4.0f, 0.0f, 0.0f});
        assert(edge);
        assert(near(edge->hit.distance, 1.6f, 1e-4f));
        assert(nearVector(edge->hit.point, Vec3{-0.4f, 1.3f, 0.0f}, 1e-4f));
        assert(nearVector(edge->hit.normal, Vec3{-0.8f, 0.6f, 0.0f}, 1e-4f));

        // Higher than the radius above the top: it passes over.
        assert(!sweepSphere(level, Sphere{Vec3{-2.0f, 1.6f, 0.0f}, 0.5f}, Vec3{4.0f, 0.0f, 0.0f}));

        // Towards a corner: the box's corner at (0, 1, -5), with the centre 0.3 above and 0.2 to the side of it.
        const float ahead = std::sqrt(0.25f - 0.09f - 0.04f);
        const auto corner = sweepSphere(level, Sphere{Vec3{-1.0f, 1.3f, -5.2f}, 0.5f}, Vec3{2.0f, 0.0f, 0.0f});
        assert(corner);
        assert(near(corner->hit.distance, 1.0f - ahead, 1e-4f));
        assert(nearVector(corner->hit.normal, Vec3{-ahead, 0.3f, -0.2f} / 0.5f, 1e-4f));
    }

    // Of two walls in the way, the nearer stops it; a thin wall stops it however fast it goes (no tunnelling).
    void testNearestAndThin() {
        StaticLevel level;
        level.add(block(Vec3{6.0f, -2.0f, -2.0f}, Vec3{6.5f, 2.0f, 2.0f}));
        level.add(block(Vec3{3.0f, -2.0f, -2.0f}, Vec3{3.01f, 2.0f, 2.0f}));

        const auto hit = sweepSphere(level, Sphere{Vec3{0.0f, 0.0f, 0.0f}, 0.1f}, Vec3{100.0f, 0.0f, 0.0f});
        assert(hit);
        assert(near(hit->hit.distance, 2.9f, 1e-4f));
        assert(nearVector(hit->hit.normal, Vec3{-1.0f, 0.0f, 0.0f}, 1e-4f));
    }

    // Already touching or overlapping something: moving into it stops at once, moving along or away from it is free.
    void testStartingInContact() {
        const StaticLevel level = flatLevel();
        const Sphere resting{Vec3{0.0f, 0.5f, 0.0f}, 0.5f};

        const auto into = sweepSphere(level, resting, Vec3{1.0f, -0.01f, 0.0f});
        assert(into && into->hit.distance == 0.0f);
        assert(nearVector(into->hit.point, resting.centre, 1e-6f));
        assert(nearVector(into->hit.normal, Vec3{0.0f, 1.0f, 0.0f}, 1e-4f));

        assert(!sweepSphere(level, resting, Vec3{1.0f, 0.0f, 0.0f}));
        assert(!sweepSphere(level, resting, Vec3{0.0f, 1.0f, 0.0f}));
        assert(!sweepSphere(level, Sphere{Vec3{0.0f, 0.4f, 0.0f}, 0.5f}, Vec3{0.0f, 1.0f, 0.0f}));

        // Not moving at all hits nothing.
        assert(!sweepSphere(level, resting, Vec3{}));
    }

    /*
    * On random levels: where the sweep says it first touches, the sphere touches the triangle it names and overlaps nothing
    * a little earlier; where it says it hits nothing, no point along the way overlaps anything. Starting points that already
    * overlap are skipped. The level query also agrees with testing every triangle on its own.
    */
    void testRandomLevels() {
        Numbers numbers;
        for (int round = 0; round < 4; ++round) {
            StaticLevel level{CollisionSettings{.gridCellSize = numbers.next(0.5f, 3.0f)}};
            level.add(rubble(numbers, 80));

            int checked = 0;
            while (checked < 25) {
                const Sphere sphere{numbers.nextPoint(7.0f), numbers.next(0.05f, 0.8f)};
                if (overlapsLevel(level, sphere, 0.0f)) {
                    continue;
                }
                ++checked;
                const Vec3 motion = numbers.nextPoint(4.0f);
                const float length = motion.length();
                const Vec3 direction = motion / length;

                const auto hit = sweepSphere(level, sphere, motion);

                std::optional<RayHit> byTriangle;
                for (std::uint32_t triangle = 0; triangle < level.getTriangleCount(); ++triangle) {
                    const auto corners = level.getTriangle(triangle);
                    const auto one = sweepSphere(sphere, motion, corners[0], corners[1], corners[2]);
                    if (one && (!byTriangle || one->distance < byTriangle->distance)) {
                        byTriangle = one;
                    }
                }
                assert(hit.has_value() == byTriangle.has_value());

                const float reach = hit ? hit->hit.distance : length;
                for (float travelled = 0.0f; travelled < reach - 2e-3f; travelled += 0.01f) {
                    assert(!overlapsLevel(level, Sphere{sphere.centre + direction * travelled, sphere.radius}, 1e-4f));
                }
                if (hit) {
                    assert(near(hit->hit.distance, byTriangle->distance, 1e-4f));
                    assert(nearVector(hit->hit.point, sphere.centre + direction * hit->hit.distance, 1e-4f));
                    const auto corners = level.getTriangle(hit->triangle);
                    const auto touching = contact(Sphere{hit->hit.point, sphere.radius + 1e-3f}, corners[0], corners[1], corners[2]);
                    assert(touching);
                    assert(nearVector(touching->normal, hit->hit.normal, 2e-2f));
                    assert(hit->hit.normal.dot(direction) <= 1e-4f);
                }
            }
        }
    }
}

void testSphereCast() {
    testFace();
    testEdgeAndCorner();
    testNearestAndThin();
    testStartingInContact();
    testRandomLevels();
}
