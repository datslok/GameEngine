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
#include "scene/terrain_heights.h"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

/*
* The plants the GPU placed (render/gpu/planting_records.h) drawn in every view (ROADMAP phase 18, the valley's stages
* 2b2 and 2c1; docs/architecture/planting.md): the camera, its velocity list, the probe faces and the shadow tiles drawn.
* A table of each kind's parts, each part's instance and cull record in the plant's own frame. Each frame each view
* takes the blocks of the cells whose box it may see (tested on the CPU: a box a held cell's drawn layer), and its
* passes cull each of those plants in the region, its parts choosing their levels as the views' own draws do
* (cameraCandidateFlags on the part's record moved to the plant), count by group (a part's level slot), sum the counts,
* write each group's indirect command and each plant's entries at their places (placed_cull_*.comp). The vertex shaders
* build each drawn part's instance from its plant (placedInstance), for draws from PLACED_INSTANCE_BASE. Render-only and
* API-free: cullPlacedOnCpu runs the passes' steps for tests and placement-compare.
*/

inline constexpr std::uint32_t placedGroupSize = PLACED_GROUP_SIZE;
inline constexpr std::size_t placedMostParts = PLACED_MOST_PARTS;
inline constexpr std::size_t placedMostGroups = PLACED_MOST_GROUPS;
inline constexpr std::uint32_t placedInstanceBase = PLACED_INSTANCE_BASE;
inline constexpr std::uint32_t placedItemRow = PLACED_ITEM_ROW;

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

static_assert(sizeof(GpuPlacedKind) == 64, "GpuPlacedKind must match placed_drawing.glsl's PlacedKind");

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

// A block one view culls: the passes run an item a workgroup, every view's at once.
struct GpuPlacedItem {
    std::uint32_t block = 0;
    std::uint32_t view = 0;
};

static_assert(sizeof(GpuPlacedItem) == 8, "GpuPlacedItem must match placed_drawing.glsl's PlacedItem");

/*
* A view's culling: the view, the region drawn (north, south, spread), its items (from firstItem), the groups, the list
* it makes (a part drawn where cameraCandidateFlags gives this bit: CAMERA_CULL_DRAWN, or CAMERA_CULL_MOVING for the
* camera's velocity list), its entries' place and room, and every view's count of views and items.
*/
struct GpuPlacedView {
    GpuCameraCull camera{};
    float region[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t firstItem = 0;
    std::uint32_t items = 0;
    std::uint32_t groups = 0;
    std::uint32_t wanted = CAMERA_CULL_DRAWN;
    std::uint32_t entryBase = 0;
    std::uint32_t entryRoom = 0;
    std::uint32_t viewCount = 0;
    std::uint32_t itemCount = 0;
};

static_assert(sizeof(GpuPlacedView) == 304, "GpuPlacedView must match placed_drawing.glsl's PlacedView");

// One drawn part: its plant's number in the slots' plants with the draw list bits (thinned, fade), and its part's.
struct GpuPlacedEntry {
    std::uint32_t plant = 0;
    std::uint32_t part = 0;

    bool operator==(const GpuPlacedEntry&) const = default;
};

static_assert(sizeof(GpuPlacedEntry) == 8, "GpuPlacedEntry must match placed_drawing.glsl's uvec2");

/*
* A layer's parts' bounds from the plant's foot, a unit of scale: the farthest across (the turn keeps it), up and down
* (each at least 0); unknown when a part's size is; whether a camera sees any part (not shadow-only or hidden) and a
* shadow takes any (casting, not hidden); its most-parted kind's parts.
*/
struct PlacedLayerReach {
    float across = 0.0f;
    float up = 0.0f;
    float down = 0.0f;
    bool unknown = false;
    bool seen = false;
    bool casts = false;
    std::uint32_t mostParts = 0;
};

/*
* Every kind's parts for the GPU: parts by style (batchByMesh's: plain, double-sided, cut-out, double-sided cut-out),
* each with a group for each of its level slots (a meshless slot's group draws nothing), so groups run style by style
* and every style is one multi-draw. draws[p] is part p's draw among the frame's plantedParts, to pack its instance.
* layerReach a layer each.
*/
struct PlacedTable {
    std::vector<GpuPlacedPart> parts;
    std::vector<std::size_t> draws;
    std::vector<GpuPlacedKind> kinds;
    std::vector<GpuPlacedGroup> groups;
    std::vector<CullGroupStyle> styles;
    CullDrawOrder order;
    std::vector<bool> drawnLayers;
    std::vector<PlacedLayerReach> layerReach;
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

// A held cell's drawn layer's blocks, one run each, in placedBlocks' order.
struct PlacedBlockRun {
    PlantingCell cell;
    std::uint32_t layer = 0;
    std::uint32_t firstBlock = 0;
    std::uint32_t blocks = 0;
};

std::vector<PlacedBlockRun> placedBlockRuns(const std::map<PlantingCell, int>& held, const PlantingSlotLayout& layout, const PlacedTable& table);

/*
* The box every part of a layer's plants in a cell may reach (chunkInView tests it, as the scene's chunks): the cell
* grown by the reach across at the layer's largest scale, from the ground's lowest less the reach down to its highest
* plus the reach up (feet sunk by the layer's footSink), grown by the chunks' margin; flags as the layer's parts are
* seen and cast, always taken for a part of unknown size.
*/
GpuCullChunk placedCellChunk(const PlantingBox& cell, const TerrainHeightRange& ground, const PlantingLayerRecord& layer, const PlacedLayerReach& reach);

// A view asked for: its culling view and the list it makes (GpuPlacedView::wanted).
struct PlacedViewAsked {
    GpuCameraCull camera{};
    std::uint32_t wanted = CAMERA_CULL_DRAWN;
};

/*
* The views' records and items: each view the blocks of each run whose box (chunks, a run each) it may see, in run
* order; its entries after the view before's, room for two a part of its layer's most-parted kind a plant (a part
* fading draws two; one in a shadow tile, where nothing fades). entries: every view's room.
*/
struct PlacedViews {
    std::vector<GpuPlacedView> views;
    std::vector<GpuPlacedItem> items;
    std::uint32_t entries = 0;
};

PlacedViews placedViews(std::span<const PlacedViewAsked> asked, std::span<const PlacedBlockRun> runs, std::span<const GpuCullChunk> chunks,
                        const PlantedRegion& region, const PlacedTable& table);

/*
* What a cell made or dropped stands for in the shadow tiles' tests (shadowTileNeeds): the sphere round its casting
* layers' boxes, casting into every cascade; no casting layer, no flags (no tile takes it); a box always taken, a sphere
* of unknown size.
*/
GpuCullRecord placedChangeRecord(std::span<const GpuCullChunk> chunks);

// What the cached shadow tiles depend on in the table and region: its parts' records and levels, its kinds and groups.
std::uint64_t placedTableSignature(const PlacedTable& table, const PlantedRegion& region);

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
* What the passes write for one view from the slots' plants and counts (as the planting left them) and the maps (for the
* region): each of its items' block's plants in its count, inside the region, their kinds' parts flagged by
* cameraCandidateFlags at every level slot with a mesh, each one with the view's wanted bit an entry in its group. The
* entries from the view's own (entryBase), the commands' first instances counting from there.
*/
PlacedCullResult cullPlacedOnCpu(const PlacedTable& table, std::span<const GpuPlacedBlock> blocks, std::span<const GpuPlacedItem> items,
                                 std::span<const GpuPlacedPlant> plants, std::span<const std::uint32_t> counts, const PlantingMaps& maps,
                                 const GpuPlacedView& view);

// ---- placement-compare's placed draws ----

/*
* A view's GPU entries and commands against the CPU's, group by group: entries in both, the CPU's the GPU lacks, the
* GPU's the CPU lacks (of those, parts whose choice is within rounding of changing: nearACameraEdge on the placed record),
* groups whose entries agree but in another order, and commands differing. gpuCommands are the view's, gpuEntries every
* view's (its commands count into them from the start).
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
