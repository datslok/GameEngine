#pragma once

#include "math/mat4.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/cull_records.h"
#include "render/gpu/gpu_scene.h"
#include "render/gpu/mesh_pool.h"
#include "render/gpu/placed_drawing_shared.h"
#include "render/gpu/planting_records.h"
#include "scene/frame_description.h"
#include "scene/planting_cells.h"
#include "scene/planting_maps.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

/*
* The plants the GPU placed (render/gpu/planting_records.h) drawn in the camera's prepass and main pass (ROADMAP phase
* 18, the valley's stage 2b2; docs/architecture/planting.md): a table of each kind's parts, each part's instance and cull
* record in the plant's own frame; each frame every plant of the held slots in the region culled, its parts choosing
* their levels as the camera's own draws do (cameraCandidateFlags on the part's record moved to the plant), counted by
* group (a part's level slot), the counts summed, each group's indirect command written and each plant's entries
* written at their places (placed_cull_*.comp). The vertex shaders build each drawn part's instance from its plant
* (placedInstance), for draws from PLACED_INSTANCE_BASE. Render-only and API-free: cullPlacedOnCpu runs the passes'
* steps for tests and placement-compare.
*/

inline constexpr std::uint32_t placedGroupSize = PLACED_GROUP_SIZE;
inline constexpr std::size_t placedMostParts = PLACED_MOST_PARTS;
inline constexpr std::size_t placedMostGroups = PLACED_MOST_GROUPS;
inline constexpr std::uint32_t placedInstanceBase = PLACED_INSTANCE_BASE;

// One part of a kind: its cull record and level table in the plant's frame, and its first level slot's group (slot k's
// is firstGroup + k). Must match placed_drawing.glsl's PlacedPart.
struct GpuPlacedPart {
    GpuCullRecord record{};
    GpuMeshLevels levels{};
    std::uint32_t firstGroup = 0;
    std::uint32_t unused[3] = {0, 0, 0};
};

static_assert(sizeof(GpuPlacedPart) == 112, "GpuPlacedPart must match placed_drawing.glsl's PlacedPart");

// A layer's kind: how many parts (none: not drawn) and which, in its own order (the table's parts run by style, so a
// kind's are not one run). The table holds one a layer's PLANTING_MOST_KINDS kinds.
struct GpuPlacedKind {
    std::uint32_t parts = 0;
    std::uint32_t unused[3] = {0, 0, 0};
    std::uint32_t part[PLACED_MOST_PARTS] = {};
};

static_assert(sizeof(GpuPlacedKind) == 48, "GpuPlacedKind must match placed_drawing.glsl's PlacedKind");

// A workgroup's plants: from firstPlant in the slots' plants, its slot's layer count at countPlace, the block's first
// plant within its layer's run, and its layer's kinds in the kind table.
struct GpuPlacedBlock {
    std::uint32_t firstPlant = 0;
    std::uint32_t countPlace = 0;
    std::uint32_t firstInLayer = 0;
    std::uint32_t firstKind = 0;
};

static_assert(sizeof(GpuPlacedBlock) == 16, "GpuPlacedBlock must match placed_drawing.glsl's PlacedBlock");

// A group: its mesh's indices in the shared mesh buffer. Must match placed_drawing.glsl's PlacedGroup.
struct GpuPlacedGroup {
    std::uint32_t indexCount = 0;
    std::uint32_t firstIndex = 0;
    std::int32_t vertexOffset = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuPlacedGroup) == 16, "GpuPlacedGroup must match placed_drawing.glsl's PlacedGroup");

// The frame's culling: the camera's view, the region drawn (north, south, spread), the blocks, groups and the
// entries' room.
struct GpuPlacedView {
    GpuCameraCull camera{};
    float region[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t blocks = 0;
    std::uint32_t groups = 0;
    std::uint32_t entryRoom = 0;
    std::uint32_t unused = 0;
};

static_assert(sizeof(GpuPlacedView) == 288, "GpuPlacedView must match placed_drawing.glsl's PlacedView");

// One drawn part: its plant's number in the slots' plants with the draw list bits (thinned, fade), and its part's.
struct GpuPlacedEntry {
    std::uint32_t plant = 0;
    std::uint32_t part = 0;

    bool operator==(const GpuPlacedEntry&) const = default;
};

static_assert(sizeof(GpuPlacedEntry) == 8, "GpuPlacedEntry must match placed_drawing.glsl's uvec2");

/*
* Every kind's parts for the GPU: parts by style (batchByMesh's: plain, double-sided, cut-out, double-sided cut-out),
* each with a group for each of its level slots (a meshless slot's group draws nothing), so groups run style by style
* and every style is one multi-draw. draws[p] is part p's draw among the frame's plantedParts, to pack its instance.
*/
struct PlacedTable {
    std::vector<GpuPlacedPart> parts;
    std::vector<std::size_t> draws;
    std::vector<GpuPlacedKind> kinds;
    std::vector<GpuPlacedGroup> groups;
    std::vector<CullGroupStyle> styles;
    CullDrawOrder order;
    std::vector<bool> drawnLayers;
    std::uint32_t mostParts = 0;
};

/*
* The table of the frame's planted parts and kinds over layerCount layers. Throws std::invalid_argument for a kind
* past PLANTING_MOST_KINDS or a layer past layerCount, more than placedMostParts parts in a kind, glass, a mesh not in
* the pools, more than three levels, or more than placedMostGroups groups.
*/
PlacedTable buildPlacedTable(std::span<const DrawItem> parts, std::span<const PlantedKindParts> kinds, std::size_t layerCount,
                             const std::function<MeshPlacement(MeshHandle)>& placement);

// The blocks over every held slot's drawn layers, slot by slot (in the held map's order) and layer by layer, each layer
// cut into blocks of placedGroupSize of its room in the slot.
std::vector<GpuPlacedBlock> placedBlocks(const std::map<PlantingCell, int>& held, const PlantingSlotLayout& layout, const PlacedTable& table);

// The entries' room: two a part of the most-parted kind for every plant a block may hold (a part fading draws two).
std::uint32_t placedEntryRoom(std::size_t blocks, const PlacedTable& table);

GpuPlacedView packPlacedView(const GpuCameraCull& camera, const PlantedRegion& region, std::size_t blocks, const PlacedTable& table);

// ---- Mirrored by placed_drawing.glsl ----

// The plant's model matrix: as Transform{foot, Quat::fromAxisAngle(up, turn), scale}.getMatrix() makes it, bit for bit.
Mat4 placedPlantModel(const GpuPlacedPlant& plant);

// A sphere in the plant's frame (xyz centre, w radius, below 0 unknown) placed in the world.
std::array<float, 4> placedSphere(const float sphere[4], const GpuPlacedPlant& plant);

// A part's record in the plant's frame placed in the world: its spheres moved, the rest kept.
GpuCullRecord placedCullRecord(const GpuCullRecord& part, const GpuPlacedPlant& plant);

// A part's instance in the plant's frame placed in the world, as the vertex shaders make it: the plant's matrix, its
// rotation for the normals (normalised in the shaders), its scale, its level sphere moved.
GpuInstance placedInstance(const GpuInstance& part, const GpuPlacedPlant& plant);

// ---- The passes' steps on the CPU ----

// The groups' commands (firstInstance from placedInstanceBase) and the entries, group by group, each group's in block
// then plant order.
struct PlacedCullResult {
    std::vector<GpuDrawCommand> commands;
    std::vector<GpuPlacedEntry> entries;
};

/*
* What the passes write from the slots' plants and counts (as the planting left them) and the maps (for the region):
* each block's plants in its count, inside the region, their kinds' parts flagged by cameraCandidateFlags at every level
* slot with a mesh, each drawn one an entry in its group.
*/
PlacedCullResult cullPlacedOnCpu(const PlacedTable& table, std::span<const GpuPlacedBlock> blocks, std::span<const GpuPlacedPlant> plants,
                                 std::span<const std::uint32_t> counts, const PlantingMaps& maps, const GpuPlacedView& view);

// ---- placement-compare's placed draws ----

/*
* The GPU's entries and commands against the CPU's, group by group: entries in both, the CPU's the GPU lacks, the GPU's
* the CPU lacks (of those, parts whose choice is within rounding of changing: nearACameraEdge on the placed record),
* groups whose entries agree but in another order, and commands differing.
*/
struct PlacedComparison {
    std::size_t entries = 0;
    std::size_t matching = 0;
    std::size_t missing = 0;
    std::size_t extra = 0;
    std::size_t nearAnEdge = 0;
    std::size_t reordered = 0;
    std::size_t commandsDiffering = 0;
};

PlacedComparison comparePlacedDraws(const PlacedTable& table, std::span<const GpuPlacedPlant> plants, const GpuPlacedView& view, const PlacedCullResult& cpu,
                                    std::span<const GpuDrawCommand> gpuCommands, std::span<const GpuPlacedEntry> gpuEntries);

std::string describePlacedComparison(const PlacedComparison& comparison);
