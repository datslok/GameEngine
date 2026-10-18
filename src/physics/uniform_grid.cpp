#include "physics/uniform_grid.h"

#include "physics/overlap.h"

#include <cmath>
#include <stdexcept>

namespace {
    // Beyond this many cells from the origin, cell coordinates could overflow an int while stepping.
    constexpr float farthestCell = 1.0e9f;

    bool isFinite(const Vec3& point) {
        return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
    }
}

GridCell cellOf(const Vec3& point, float cellSize) {
    return GridCell{
        static_cast<int>(std::floor(point.x / cellSize)),
        static_cast<int>(std::floor(point.y / cellSize)),
        static_cast<int>(std::floor(point.z / cellSize))
    };
}

std::size_t UniformGrid::CellHash::operator()(const GridCell& cell) const {
    const auto mix = [](int value, std::size_t prime) { return static_cast<std::size_t>(static_cast<unsigned int>(value)) * prime; };
    return mix(cell.x, 73856093u) ^ mix(cell.y, 19349663u) ^ mix(cell.z, 83492791u);
}

UniformGrid::UniformGrid(float cellSize, int largeItemCells)
    : cellSize(cellSize), largeItemCells(largeItemCells) {
    if (!std::isfinite(cellSize) || cellSize <= 0.0f) {
        throw std::invalid_argument("Grid cells must have a finite, positive size");
    }
}

float UniformGrid::getCellSize() const {
    return cellSize;
}

std::size_t UniformGrid::getItemCount() const {
    return itemCount;
}

/*
* Emptying each cell's list keeps its memory for the next fill. A cell that was already empty (nothing used it since the
* last clear) is removed instead, so cells that things have moved away from do not pile up forever.
*/
void UniformGrid::clear() {
    for (auto cell = cells.begin(); cell != cells.end();) {
        if (cell->second.empty()) {
            cell = cells.erase(cell);
        } else {
            cell->second.clear();
            ++cell;
        }
    }
    largeItems.clear();
    itemCount = 0;
    anyCells = false;
}

/*
* The item goes into every cell its box touches, or onto the large list if that would be too many cells.
*/
void UniformGrid::insert(std::uint32_t item, const Aabb& bounds) {
    if (!isFinite(bounds.minimum) || !isFinite(bounds.maximum)) {
        throw std::invalid_argument("A grid item's box must be finite");
    }
    const float reach = farthestCell * cellSize;
    if (std::abs(bounds.minimum.x) > reach || std::abs(bounds.minimum.y) > reach || std::abs(bounds.minimum.z) > reach ||
        std::abs(bounds.maximum.x) > reach || std::abs(bounds.maximum.y) > reach || std::abs(bounds.maximum.z) > reach) {
        throw std::invalid_argument("A grid item is too far from the origin for its cell size");
    }

    if (item >= itemBounds.size()) {
        itemBounds.resize(item + 1);
        seenBy.resize(item + 1, 0);
    }
    itemBounds[item] = bounds;
    ++itemCount;

    const GridCell low = cellOf(bounds.minimum, cellSize);
    const GridCell high = cellOf(bounds.maximum, cellSize);
    const long long cellCount = static_cast<long long>(high.x - low.x + 1)
        * static_cast<long long>(high.y - low.y + 1)
        * static_cast<long long>(high.z - low.z + 1);

    if (cellCount > largeItemCells) {
        largeItems.push_back(item);
        return;
    }

    for (int x = low.x; x <= high.x; ++x) {
        for (int y = low.y; y <= high.y; ++y) {
            for (int z = low.z; z <= high.z; ++z) {
                cells[GridCell{x, y, z}].push_back(item);
            }
        }
    }

    if (!anyCells) {
        lowestCell = low;
        highestCell = high;
        anyCells = true;
    } else {
        lowestCell = GridCell{std::min(lowestCell.x, low.x), std::min(lowestCell.y, low.y), std::min(lowestCell.z, low.z)};
        highestCell = GridCell{std::max(highestCell.x, high.x), std::max(highestCell.y, high.y), std::max(highestCell.z, high.z)};
    }
}

/*
* Visit the cells the region touches (only within the occupied range) and keep each item met for the first time whose own
* box overlaps the region: sharing a cell is only a hint, since a cell is bigger than most of what it holds.
*/
void UniformGrid::query(const Aabb& region, std::vector<std::uint32_t>& found) const {
    found.clear();
    const std::uint32_t query = startQuery();

    for (const std::uint32_t item : largeItems) {
        if (firstMeeting(item, query) && overlaps(itemBounds[item], region)) {
            found.push_back(item);
        }
    }

    if (!anyCells || !isFinite(region.minimum) || !isFinite(region.maximum)) {
        return;
    }

    const GridCell regionLow = cellOf(region.minimum, cellSize);
    const GridCell regionHigh = cellOf(region.maximum, cellSize);
    const GridCell low{std::max(regionLow.x, lowestCell.x), std::max(regionLow.y, lowestCell.y), std::max(regionLow.z, lowestCell.z)};
    const GridCell high{std::min(regionHigh.x, highestCell.x), std::min(regionHigh.y, highestCell.y), std::min(regionHigh.z, highestCell.z)};

    for (int x = low.x; x <= high.x; ++x) {
        for (int y = low.y; y <= high.y; ++y) {
            for (int z = low.z; z <= high.z; ++z) {
                const std::vector<std::uint32_t>* items = itemsIn(GridCell{x, y, z});
                if (!items) {
                    continue;
                }
                for (const std::uint32_t item : *items) {
                    if (firstMeeting(item, query) && overlaps(itemBounds[item], region)) {
                        found.push_back(item);
                    }
                }
            }
        }
    }
}

/*
* Numbers count up, so an item marked by an earlier query never looks "met" by this one. When the count wraps around
* (after four billion queries) every mark is reset, so an old mark cannot be mistaken for a new one.
*/
std::uint32_t UniformGrid::startQuery() const {
    ++queryNumber;
    if (queryNumber == 0) {
        std::fill(seenBy.begin(), seenBy.end(), 0);
        queryNumber = 1;
    }
    return queryNumber;
}

bool UniformGrid::firstMeeting(std::uint32_t item, std::uint32_t query) const {
    if (seenBy[item] == query) {
        return false;
    }
    seenBy[item] = query;
    return true;
}

const std::vector<std::uint32_t>* UniformGrid::itemsIn(const GridCell& cell) const {
    const auto found = cells.find(cell);
    return found == cells.end() ? nullptr : &found->second;
}
