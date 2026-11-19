// The placed drawing's passes' shared part (render/vulkan/vulkan_placed_drawing.h): the planting's buffers (its slots
// read), the table, blocks, view and the passes' own buffers, and a block's plant as the passes take it.
#ifndef PLACED_CULL_GLSL
#define PLACED_CULL_GLSL

#define PLANTING_SLOTS_READ
#include "planting_cells.glsl"
#include "placed_drawing.glsl"

layout(std430, set = 0, binding = PLACED_PARTS_BINDING) readonly buffer PlacedParts {
    PlacedPart placedParts[];
};

// PLANTING_MOST_KINDS a layer.
layout(std430, set = 0, binding = PLACED_KINDS_BINDING) readonly buffer PlacedKinds {
    PlacedKind placedKinds[];
};

layout(std430, set = 0, binding = PLACED_BLOCKS_BINDING) readonly buffer PlacedBlocks {
    PlacedBlock placedBlocks[];
};

layout(std430, set = 0, binding = PLACED_VIEW_BINDING) readonly buffer PlacedViews {
    PlacedView placedView;
};

layout(std430, set = 0, binding = PLACED_GROUPS_BINDING) readonly buffer PlacedGroups {
    PlacedGroup placedGroups[];
};

// Each block's count of each group (block * groups + group), made each block's first place within its group by the
// scan.
layout(std430, set = 0, binding = PLACED_BLOCK_COUNTS_BINDING) buffer PlacedBlockCounts {
    uint placedBlockCounts[];
};

// Each group's total, then each group's first entry (after PLACED_MOST_GROUPS).
layout(std430, set = 0, binding = PLACED_TOTALS_BINDING) buffer PlacedTotals {
    uint placedTotals[];
};

layout(std430, set = 0, binding = PLACED_COMMANDS_BINDING) writeonly buffer PlacedCommands {
    DrawCommand placedCommands[];
};

layout(std430, set = 0, binding = PLACED_ENTRIES_BINDING) writeonly buffer PlacedEntries {
    uvec2 placedEntries[];
};

// The thread's plant: its block's, held in the slot's count and standing in the region; its number in the slots.
bool placedBlockPlant(PlacedBlock block, uint thread, out uint number, out PlacedPlant plant) {
    uint index = block.firstInLayer + thread;
    number = block.firstPlant + thread;
    plant = PlacedPlant(vec3(0.0), 0.0, 0.0, 0.0, 0u, 0.0);

    if (index >= plantingCounts[block.countPlace]) {
        return false;
    }

    plant = placedPlants[number];
    return plantingInsideRegion(placedView.region.xyz, plant.foot);
}

#endif
