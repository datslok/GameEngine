#pragma once

#include "math/aabb.h"
#include "math/ray.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <vector>

// One cube of the grid, by its integer coordinates: the cell (i, j, k) covers [i s, (i + 1) s) on x, and so on.
struct GridCell {
    int x = 0;
    int y = 0;
    int z = 0;

    bool operator==(const GridCell& other) const = default;
};

// The cell a point is in: floor(coordinate / cellSize) on each axis. Floor, not truncation, so -0.5 is in cell -1.
GridCell cellOf(const Vec3& point, float cellSize);

// How much of the grid a ray walk looked at, for the debug readout and tests.
struct GridRayWalk {
    int cells = 0; // cells stepped through
    int items = 0; // items handed to the visitor (each once)
};

/*
* A broad phase: space cut into cubes of one size, each listing the items whose bounding box touches it, so a query only
* looks at items in the cells it touches instead of at every item. Items are numbers (the caller keeps what they stand for)
* with a box each. Only occupied cells are stored, in a hash map, so the world can be any size.
*
* Items whose box would cover more than largeItemCells cells (a whole floor) go on a separate list that every query checks
* directly, rather than filling hundreds of cells.
*
* Queries mark each item they see with the query's number ("mailboxing"), so an item in many cells is reported once. Those
* marks are changed by const queries, so one grid must not be queried from two threads at once.
*/
class UniformGrid {
public:
    explicit UniformGrid(float cellSize, int largeItemCells = 64);

    float getCellSize() const;
    std::size_t getItemCount() const;

    // Forget every item, keeping the memory for the next fill (allocating is slow; a grid rebuilt each tick reuses it).
    void clear();

    // Item numbers should be small and dense (0, 1, 2, ...): they index the marks. Throws std::invalid_argument for a box
    // that is not finite or too far out for integer cell coordinates.
    void insert(std::uint32_t item, const Aabb& bounds);

    // Replaces found with every item whose box overlaps region, each once, in no particular order.
    void query(const Aabb& region, std::vector<std::uint32_t>& found) const;

    /*
    * Hands visit(item, reach) every item in the cells the ray passes through, near to far, each once; large items first.
    * visit returns the reach from then on (the distance of the nearest hit it has found, or the reach it was given), in
    * world units along the ray. The walk stops when the next cell starts beyond the reach: nothing only in later cells can
    * be hit nearer. A ray that leaves the occupied cells stops too, so an unlimited reach is fine.
    */
    template <typename Visit>
    GridRayWalk walkRay(const Ray& ray, float reach, Visit&& visit) const;

private:
    // Spreads cell coordinates over the hash map's buckets: each multiplied by a large prime, then mixed with xor
    // (Teschner et al. 2003, "Optimized Spatial Hashing for Collision Detection of Deformable Objects").
    struct CellHash {
        std::size_t operator()(const GridCell& cell) const;
    };

    float cellSize;
    int largeItemCells;

    std::unordered_map<GridCell, std::vector<std::uint32_t>, CellHash> cells;
    std::vector<std::uint32_t> largeItems;
    std::vector<Aabb> itemBounds; // by item number
    std::size_t itemCount = 0;

    // The range of cells holding at least one (not large) item; queries and ray walks never look outside it.
    bool anyCells = false;
    GridCell lowestCell;
    GridCell highestCell;

    // Mailboxing: seenBy[item] is the number of the last query that met the item.
    mutable std::vector<std::uint32_t> seenBy;
    mutable std::uint32_t queryNumber = 0;

    // A fresh query number, so earlier queries' marks no longer count.
    std::uint32_t startQuery() const;

    // True the first time a query meets an item, false after; marks it as met.
    bool firstMeeting(std::uint32_t item, std::uint32_t query) const;

    const std::vector<std::uint32_t>* itemsIn(const GridCell& cell) const;
};

/*
* Amanatides and Woo, "A Fast Voxel Traversal Algorithm for Ray Tracing" (1987). Along each axis the ray crosses a cell
* boundary every tDelta = cellSize / |direction| of distance, so after the first crossing (tMax) the crossings are evenly
* spaced. Each step moves into the neighbouring cell through whichever boundary comes first (the smallest tMax) and pushes
* that axis's tMax on by its tDelta, which visits exactly the cells the ray passes through, in order.
*
* Before walking, the ray is clipped to the box of occupied cells, so it starts in (or at the edge of) a cell that may hold
* something and stops when it leaves them, which also ends rays that point into empty space forever.
*/
template <typename Visit>
GridRayWalk UniformGrid::walkRay(const Ray& ray, float reach, Visit&& visit) const {
    GridRayWalk walk;
    const std::uint32_t query = startQuery();

    const auto meet = [&](std::uint32_t item) {
        if (firstMeeting(item, query)) {
            ++walk.items;
            reach = visit(item, reach);
        }
    };

    // Large items can be anywhere, so they are met before the walk.
    for (const std::uint32_t item : largeItems) {
        meet(item);
    }

    const float length = ray.direction.length();
    if (!anyCells || !(length > 0.0f)) {
        return walk;
    }
    const Vec3 direction = ray.direction / length;

    const float origin[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
    const float step[3] = {direction.x, direction.y, direction.z};
    const int lowest[3] = {lowestCell.x, lowestCell.y, lowestCell.z};
    const int highest[3] = {highestCell.x, highestCell.y, highestCell.z};

    // Clip to the occupied box with the slab method: the ray is inside where all three axes' stretches overlap.
    float enter = 0.0f;
    float exit = reach;
    for (int axis = 0; axis < 3; ++axis) {
        const float low = static_cast<float>(lowest[axis]) * cellSize;
        const float high = static_cast<float>(highest[axis] + 1) * cellSize;

        if (step[axis] == 0.0f) {
            if (origin[axis] < low || origin[axis] > high) {
                return walk;
            }
            continue;
        }

        const float toLow = (low - origin[axis]) / step[axis];
        const float toHigh = (high - origin[axis]) / step[axis];
        enter = std::max(enter, std::min(toLow, toHigh));
        exit = std::min(exit, std::max(toLow, toHigh));
    }
    if (enter > exit) {
        return walk;
    }

    // The first cell, kept inside the occupied range (a start exactly on the box's far side would otherwise round outside).
    const GridCell start = cellOf(ray.origin + direction * enter, cellSize);
    int cell[3] = {
        std::clamp(start.x, lowest[0], highest[0]),
        std::clamp(start.y, lowest[1], highest[1]),
        std::clamp(start.z, lowest[2], highest[2])
    };

    constexpr float never = std::numeric_limits<float>::infinity();
    int cellStep[3];
    float nextCrossing[3]; // tMax: the distance along the ray at which it next crosses a boundary on this axis
    float crossingGap[3];  // tDelta: the distance between successive crossings on this axis

    for (int axis = 0; axis < 3; ++axis) {
        if (step[axis] > 0.0f) {
            cellStep[axis] = 1;
            nextCrossing[axis] = (static_cast<float>(cell[axis] + 1) * cellSize - origin[axis]) / step[axis];
            crossingGap[axis] = cellSize / step[axis];
        } else if (step[axis] < 0.0f) {
            cellStep[axis] = -1;
            nextCrossing[axis] = (static_cast<float>(cell[axis]) * cellSize - origin[axis]) / step[axis];
            crossingGap[axis] = -cellSize / step[axis];
        } else {
            cellStep[axis] = 0;
            nextCrossing[axis] = never;
            crossingGap[axis] = never;
        }
    }

    while (true) {
        ++walk.cells;
        if (const std::vector<std::uint32_t>* items = itemsIn(GridCell{cell[0], cell[1], cell[2]})) {
            for (const std::uint32_t item : *items) {
                meet(item);
            }
        }

        // Step through the nearest boundary, unless the next cell starts beyond what can still win.
        int axis = 0;
        if (nextCrossing[1] < nextCrossing[axis]) {
            axis = 1;
        }
        if (nextCrossing[2] < nextCrossing[axis]) {
            axis = 2;
        }
        if (nextCrossing[axis] > reach) {
            break;
        }

        cell[axis] += cellStep[axis];
        nextCrossing[axis] += crossingGap[axis];
        if (cell[axis] < lowest[axis] || cell[axis] > highest[axis]) {
            break;
        }
    }

    return walk;
}
