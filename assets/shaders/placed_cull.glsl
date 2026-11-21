// The placed drawing's passes' shared part (render/vulkan/vulkan_placed_drawing.h): the planting's buffers (its slots
// read), the table, blocks, views and items, the passes' own buffers, an item's plant as the passes take it.
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

// Every view, each with every view's count of views and items.
layout(std430, set = 0, binding = PLACED_VIEW_BINDING) readonly buffer PlacedViews {
    PlacedView placedViews[];
};

layout(std430, set = 0, binding = PLACED_GROUPS_BINDING) readonly buffer PlacedGroups {
    PlacedGroup placedGroups[];
};

// Each item's count of each group (item * groups + group), made the item's first place within its view's group by the
// scan.
layout(std430, set = 0, binding = PLACED_BLOCK_COUNTS_BINDING) buffer PlacedBlockCounts {
    uint placedBlockCounts[];
};

// Each view's groups' totals (view * groups + group), then their first entries (from viewCount * groups).
layout(std430, set = 0, binding = PLACED_TOTALS_BINDING) buffer PlacedTotals {
    uint placedTotals[];
};

// Each view's groups' commands (view * groups + group).
layout(std430, set = 0, binding = PLACED_COMMANDS_BINDING) writeonly buffer PlacedCommands {
    DrawCommand placedCommands[];
};

layout(std430, set = 0, binding = PLACED_ENTRIES_BINDING) writeonly buffer PlacedEntries {
    uvec2 placedEntries[];
};

// Every view's blocks, view after view.
layout(std430, set = 0, binding = PLACED_ITEMS_BINDING) readonly buffer PlacedItems {
    PlacedItem placedItems[];
};

// The item a workgroup of the item passes takes: rows of PLACED_ITEM_ROW, past the last none (every view's itemCount).
bool placedWorkgroupItem(out uint item) {
    item = gl_WorkGroupID.y * PLACED_ITEM_ROW + gl_WorkGroupID.x;
    return item < placedViews[0].itemCount;
}

// The thread's plant: its block's, held in the slot's count; its number in the slots.
bool placedBlockPlant(PlacedBlock block, uint thread, out uint number, out PlacedPlant plant) {
    uint index = block.firstInLayer + thread;
    number = block.firstPlant + thread;
    plant = PlacedPlant(vec3(0.0), 0.0, 0.0, 0.0, 0u, 0.0);

    if (index >= plantingCounts[block.countPlace]) {
        return false;
    }

    plant = placedPlants[number];
    return true;
}

#endif
