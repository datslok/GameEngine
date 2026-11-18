#pragma once

#include "core/settings_visitor.h"
#include "math/vec3.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <vector>

/*
* The world cut into square cells on the ground (docs/research/GPUDRIVEN.md, stage 7): the unit the still draws are laid
* out by (scene/still_draws.h), so a change rebuilds one cell's draws rather than the world's, and later the unit worlds
* stream by (ROADMAP phase 13). 128 m as Unreal's World Partition and Spider-Man's tiles.
*/
struct CellSettings {
    // A cell's side in metres. A change lays every still draw out again.
    float size = 128.0f;

    // Room a cell's block keeps beyond its draws, as a share of them, so entities added later fit without moving it.
    float spareShare = 0.25f;

    // The smallest block a cell is given, in draws.
    int fewestSlots = 64;

    bool operator==(const CellSettings&) const = default;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(CellSettings& settings, SettingsVisitor& visitor);

/*
* A cell by its column (x) and row (z), counted from the world's origin, negative to the west and north. 64-bit, so a
* key outlasts what a float position can place (phase 13's double positions).
*/
struct CellKey {
    std::int64_t x = 0;
    std::int64_t z = 0;

    auto operator<=>(const CellKey&) const = default;
};

// The cell holding a point (its x and z), with cells size metres a side.
CellKey worldCellOf(const Vec3& position, float size);

// The draws a cell with needed draws is given room for: needed and its spare share, at least the fewest.
std::size_t cellBlockSlots(std::size_t needed, const CellSettings& settings);

/*
* Runs of slots handed out and given back (the cells' blocks in the still draws): the first free run long enough, lowest
* first, else at the end; given back runs join their free neighbours, and free runs at the end are dropped from it, so
* the slots in use stay packed towards the start. Deterministic: the same calls give the same places.
*/
class SlotBlocks {
public:
    // The first slot of a new run of count slots (count above 0).
    std::size_t take(std::size_t count);

    // A run given back (first and count as taken).
    void giveBack(std::size_t first, std::size_t count);

    // One past the last slot in use.
    std::size_t end() const;

    // Every run free, none in use.
    void clear();

private:
    struct Run {
        std::size_t first = 0;
        std::size_t count = 0;
    };

    std::vector<Run> free; // by first, never touching
    std::size_t last = 0;
};
