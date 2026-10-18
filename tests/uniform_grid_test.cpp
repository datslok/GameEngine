#include "physics/raycast.h"
#include "physics/uniform_grid.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
    // A small repeatable generator, so the random comparisons test the same cases every run.
    class Numbers {
    public:
        float next(float low, float high) {
            state = state * 6364136223846793005ull + 1442695040888963407ull;
            const float unit = static_cast<float>(state >> 40) / static_cast<float>(1ull << 24);
            return low + (high - low) * unit;
        }

    private:
        std::uint64_t state = 2024;
    };

    Aabb cubeAround(const Vec3& centre, float half) {
        return Aabb{centre - Vec3{half, half, half}, centre + Vec3{half, half, half}};
    }

    std::vector<std::uint32_t> sorted(std::vector<std::uint32_t> items) {
        std::sort(items.begin(), items.end());
        return items;
    }

    // Walks the ray with unlimited reach and returns the items in the order they were met.
    std::vector<std::uint32_t> itemsAlong(const UniformGrid& grid, const Ray& ray) {
        std::vector<std::uint32_t> met;
        grid.walkRay(ray, unlimitedRayDistance, [&](std::uint32_t item, float reach) {
            met.push_back(item);
            return reach;
        });
        return met;
    }

    void testCells() {
        // Floor, not truncation: just below zero is cell -1, not cell 0.
        assert((cellOf(Vec3{0.5f, 0.0f, 1.99f}, 2.0f) == GridCell{0, 0, 0}));
        assert((cellOf(Vec3{-0.5f, -2.0f, -2.01f}, 2.0f) == GridCell{-1, -1, -2}));
        assert((cellOf(Vec3{4.0f, 6.5f, -4.0f}, 2.0f) == GridCell{2, 3, -2}));
    }

    void testRegionQueries() {
        UniformGrid grid{2.0f};
        grid.insert(0, cubeAround(Vec3{0.0f, 0.0f, 0.0f}, 0.4f));
        grid.insert(1, cubeAround(Vec3{10.0f, 0.0f, 0.0f}, 0.4f));
        grid.insert(2, Aabb{Vec3{-3.0f, -0.5f, -0.5f}, Vec3{3.0f, 0.5f, 0.5f}}); // spans four cells along x
        assert(grid.getItemCount() == 3);

        std::vector<std::uint32_t> found;

        // Near the origin: the small cube and the long bar, the bar once although it is in several cells.
        grid.query(cubeAround(Vec3{0.5f, 0.0f, 0.0f}, 1.0f), found);
        assert((sorted(found) == std::vector<std::uint32_t>{0, 2}));

        // Sharing a cell is not enough: the region must overlap the item's own box. (1.5, 1.5, 1.5) is in the small cube's
        // cell but clear of the cube, and above the bar.
        grid.query(cubeAround(Vec3{1.5f, 1.5f, 1.5f}, 0.2f), found);
        assert(found.empty());

        // Only the far cube, and nothing far from everything.
        grid.query(cubeAround(Vec3{10.0f, 0.0f, 0.0f}, 0.1f), found);
        assert((found == std::vector<std::uint32_t>{1}));
        grid.query(cubeAround(Vec3{100.0f, 0.0f, 0.0f}, 1.0f), found);
        assert(found.empty());

        // Repeated queries keep reporting each item once (the per-item "already seen" marks must not leak between queries).
        grid.query(cubeAround(Vec3{0.0f, 0.0f, 0.0f}, 5.0f), found);
        assert((sorted(found) == std::vector<std::uint32_t>{0, 2}));
        grid.query(cubeAround(Vec3{0.0f, 0.0f, 0.0f}, 5.0f), found);
        assert((sorted(found) == std::vector<std::uint32_t>{0, 2}));

        // Clearing empties it.
        grid.clear();
        assert(grid.getItemCount() == 0);
        grid.query(cubeAround(Vec3{0.0f, 0.0f, 0.0f}, 5.0f), found);
        assert(found.empty());

        // Positions that are not finite have no cell.
        bool threw = false;
        try {
            grid.insert(0, Aabb{Vec3{0.0f, 0.0f, 0.0f}, Vec3{std::numeric_limits<float>::infinity(), 1.0f, 1.0f}});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    void testLargeItems() {
        // With at most 8 cells per item, a 100-wide floor goes on the large list and is still found by every query that
        // overlaps it, but not by one that misses it.
        UniformGrid grid{2.0f, 8};
        grid.insert(0, Aabb{Vec3{-50.0f, -1.0f, -50.0f}, Vec3{50.0f, 0.0f, 50.0f}});
        grid.insert(1, cubeAround(Vec3{3.0f, 1.0f, 3.0f}, 0.5f));

        std::vector<std::uint32_t> found;
        grid.query(cubeAround(Vec3{3.0f, 0.5f, 3.0f}, 0.6f), found);
        assert((sorted(found) == std::vector<std::uint32_t>{0, 1}));
        grid.query(cubeAround(Vec3{3.0f, 30.0f, 3.0f}, 0.6f), found);
        assert(found.empty());

        // A ray meets it too, even far outside the small items' cells.
        assert((itemsAlong(grid, Ray{Vec3{40.0f, 5.0f, 40.0f}, Vec3{0.0f, -1.0f, 0.0f}}) == std::vector<std::uint32_t>{0}));
    }

    void testRayWalk() {
        // Three cubes in a row along x, in cells 0-1, 4-5 and 8-9.
        UniformGrid grid{2.0f};
        grid.insert(0, Aabb{Vec3{1.5f, 0.0f, 0.0f}, Vec3{2.5f, 1.0f, 1.0f}});
        grid.insert(1, Aabb{Vec3{9.5f, 0.0f, 0.0f}, Vec3{10.5f, 1.0f, 1.0f}});
        grid.insert(2, Aabb{Vec3{17.5f, 0.0f, 0.0f}, Vec3{18.5f, 1.0f, 1.0f}});

        // Near to far, each once, whichever way the ray runs, and from inside the occupied region too.
        const Ray east{Vec3{-5.0f, 0.5f, 0.5f}, Vec3{1.0f, 0.0f, 0.0f}};
        assert((itemsAlong(grid, east) == std::vector<std::uint32_t>{0, 1, 2}));
        assert((itemsAlong(grid, Ray{Vec3{25.0f, 0.5f, 0.5f}, Vec3{-1.0f, 0.0f, 0.0f}}) == std::vector<std::uint32_t>{2, 1, 0}));
        assert((itemsAlong(grid, Ray{Vec3{5.0f, 0.5f, 0.5f}, Vec3{3.0f, 0.0f, 0.0f}}) == std::vector<std::uint32_t>{1, 2}));

        // Early stop: once something is hit before the next cell begins, nothing further along is even looked at.
        std::vector<std::uint32_t> met;
        const GridRayWalk walk = grid.walkRay(east, unlimitedRayDistance, [&](std::uint32_t item, float reach) {
            met.push_back(item);
            return item == 0 ? 6.5f : reach; // the first cube is hit 6.5 along, inside cell 1, which ends at x = 4 (9 along)
        });
        assert((met == std::vector<std::uint32_t>{0}));
        assert(walk.items == 1);
        assert(walk.cells <= 3);

        // The reach limits the walk the same way.
        met.clear();
        grid.walkRay(east, 10.0f, [&](std::uint32_t item, float reach) {
            met.push_back(item);
            return reach;
        });
        assert((met == std::vector<std::uint32_t>{0}));

        // Pointing away from everything with unlimited reach: the walk ends at once instead of stepping forever.
        const GridRayWalk away = grid.walkRay(Ray{Vec3{-5.0f, 0.5f, 0.5f}, Vec3{-1.0f, 0.0f, 0.0f}}, unlimitedRayDistance,
                                              [](std::uint32_t, float reach) { return reach; });
        assert(away.items == 0);
        assert(away.cells == 0);

        // Straight up the y axis (no motion in x or z), through a stack of cubes.
        UniformGrid stack{2.0f};
        stack.insert(0, cubeAround(Vec3{0.5f, 1.0f, 0.5f}, 0.3f));
        stack.insert(1, cubeAround(Vec3{0.5f, 7.0f, 0.5f}, 0.3f));
        assert((itemsAlong(stack, Ray{Vec3{0.5f, -3.0f, 0.5f}, Vec3{0.0f, 1.0f, 0.0f}}) == std::vector<std::uint32_t>{0, 1}));
    }

    // The guarantee that matters: walking a ray never misses an item whose box the ray passes through.
    void testRayWalkMissesNothing() {
        Numbers numbers;
        for (int scene = 0; scene < 50; ++scene) {
            UniformGrid grid{numbers.next(0.5f, 3.0f)};
            std::vector<Aabb> boxes;
            for (std::uint32_t item = 0; item < 40; ++item) {
                const Vec3 centre{numbers.next(-15.0f, 15.0f), numbers.next(-5.0f, 5.0f), numbers.next(-15.0f, 15.0f)};
                const Aabb box = cubeAround(centre, numbers.next(0.1f, 2.0f));
                boxes.push_back(box);
                grid.insert(item, box);
            }

            for (int rayIndex = 0; rayIndex < 20; ++rayIndex) {
                const Ray ray{
                    Vec3{numbers.next(-20.0f, 20.0f), numbers.next(-8.0f, 8.0f), numbers.next(-20.0f, 20.0f)},
                    Vec3{numbers.next(-1.0f, 1.0f), numbers.next(-1.0f, 1.0f), numbers.next(-1.0f, 1.0f)}
                };
                const std::vector<std::uint32_t> met = sorted(itemsAlong(grid, ray));

                for (std::uint32_t item = 0; item < boxes.size(); ++item) {
                    if (raycast(ray, boxes[item])) {
                        assert(std::binary_search(met.begin(), met.end(), item));
                    }
                }

                // And each item at most once.
                assert(std::adjacent_find(met.begin(), met.end()) == met.end());
            }
        }
    }
}

void testUniformGrid() {
    testCells();
    testRegionQueries();
    testLargeItems();
    testRayWalk();
    testRayWalkMissesNothing();
}
