#pragma once

#include "math/vec3.h"
#include "render/gpu/liana_trees_shared.h"
#include "render/gpu/planting_records.h"
#include "scene/liana_shape.h"
#include "scene/planting_cells.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

/*
* The lianas' trees found among the planted plants (ROADMAP phase 21 stage 3c-iii-c-iii, docs/research/LEAFGEOMETRY.md,
* Reading the poses): the CPU hangs the lianas from trees it planted by the same maths as the GPU's cells, so it knows
* each tree's foot to the bit but not its plant number (its place in the slots' plants, packed by the cells' prefix
* sums). Each frame, before the bones are posed, liana_trees.comp finds it: a thread an anchor, the foot's cell in its
* layer's set, that cell's slot from the set's table, the slot's plants of that layer searched for the same foot. The
* draws then move the anchor as the bark of that plant's tree. Render-only and API-free: the functions below mirror
* liana_trees.glsl, and the pass's step on the CPU serves the tests and bones-compare.
*/

inline constexpr std::uint32_t lianaCellEmpty = LIANA_CELL_EMPTY;
inline constexpr float lianaTreeMatchMetres = static_cast<float>(LIANA_TREE_MATCH_METRES);

/*
* A planting layer as the lookup finds its plants: slot 0's first plant of it and the plants a slot, slot 0's count of
* it and the counts a slot (the set's layers), its set's cell size (0: no set writes it, so no tree is found), its
* set's table's first place and side. Must match liana_trees.glsl's LianaTreeLayer.
*/
struct GpuLianaTreeLayer {
    std::uint32_t firstPlant = 0;
    std::uint32_t plantsASlot = 0;
    std::uint32_t firstCount = 0;
    std::uint32_t countsASlot = 0;
    float cellMetres = 0.0f;
    std::uint32_t firstCell = 0;
    std::uint32_t side = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuLianaTreeLayer) == 32, "GpuLianaTreeLayer must match liana_trees.glsl's LianaTreeLayer");

// A place of a set's cell table: the cell held there and its slot (LIANA_CELL_EMPTY none). Must match LianaCell.
struct GpuLianaCell {
    std::int32_t column = 0;
    std::int32_t row = 0;
    std::uint32_t slot = lianaCellEmpty;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuLianaCell) == 16, "GpuLianaCell must match liana_trees.glsl's LianaCell");

// What the lookup reads besides the plants: a record a field layer, and every set's cell table one after another.
struct LianaTreeTables {
    std::vector<GpuLianaTreeLayer> layers;
    std::vector<GpuLianaCell> cells;
};

/*
* The tables for a frame's sets (each cell held at its place: lianaCellPlace in a table side a side) and fieldLayers
* layers (those no set writes left empty). Throws std::invalid_argument for a set without a side or a layer past
* fieldLayers, std::logic_error for two held cells at one place (never: a ring's held cells lie within side cells
* across, PlantingCellRing).
*/
LianaTreeTables packLianaTreeTables(std::span<const PlantingSetSlots> sets, std::size_t fieldLayers);

// ---- Mirrored by liana_trees.glsl ----

// The cell, cellMetres a side, whose box holds a foot (x, z) exactly as plantingCellBox's edges do: west in, east out.
PlantingCell lianaTreeCell(float x, float z, float cellMetres);

// A cell's place in its set's table, side a side: its column and row each modulo side (0 to side - 1, below 0 too).
std::uint32_t lianaCellPlace(const PlantingCell& cell, std::uint32_t side);

// Whether a planted plant's foot is a liana's tree's: within LIANA_TREE_MATCH_METRES.
bool lianaSameTree(const Vec3& planted, const Vec3& foot);

// ---- The pass's step on the CPU ----

/*
* The plant (in the slots' plants) a liana's anchor's tree is, end 0 its first hold's and 1 its far end's: BONE_NO_TREE
* for an anchor on no tree, past the sway's reach from the eye (boneTreeWanted: it would not be posed), in a cell not
* held, or not found among its cell's plants. As liana_trees.comp finds it.
*/
std::uint32_t lianaTreePlant(const LianaShape& shape, int end, const LianaTreeTables& tables, std::span<const GpuPlacedPlant> plants,
                             std::span<const std::uint32_t> counts, const Vec3& eye, float fadeEnd);

// ---- bones-compare ----

/*
* A frame's found plants read back against the CPU's (lianaTreePlant on the plants and counts read back with them):
* the held lianas' anchors on a planted layer's tree, those the CPU finds a plant for, those the GPU did, those the two
* agree on and those they do not.
*/
struct LianaTreeComparison {
    std::size_t anchors = 0;
    std::size_t wanted = 0;
    std::size_t found = 0;
    std::size_t matching = 0;
    std::size_t differing = 0;
};

// found: two a record, slot by slot (perSlot records a slot) as the pass wrote them; only the held records are compared.
LianaTreeComparison compareLianaTrees(const LianaShapes& shapes, std::span<const std::uint32_t> found, const LianaTreeTables& tables,
                                      std::span<const GpuPlacedPlant> plants, std::span<const std::uint32_t> counts, const Vec3& eye, float fadeEnd);

std::string describeLianaTreeComparison(const LianaTreeComparison& comparison);
