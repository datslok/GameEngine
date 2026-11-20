#ifndef RENDER_GPU_PLACED_DRAWING_SHARED_H
#define RENDER_GPU_PLACED_DRAWING_SHARED_H

// The placed plants' drawing (render/gpu/placed_drawing.h, placed_drawing.glsl and the placed_cull_*.comp passes). Only
// #defines of plain numbers, since GLSL includes this file too.

// Plants a workgroup of the counting and writing passes (a block), and threads of the scans.
#define PLACED_GROUP_SIZE 64
#define PLACED_SCAN_GROUP_SIZE 256

// Workgroups a row of the item passes' dispatch (every device takes at least 65,535 a dimension): item = row * this + x.
#define PLACED_ITEM_ROW 32768

// The most parts a kind of plant is drawn with, and the most groups (a part's level slots) of every kind together.
#define PLACED_MOST_PARTS 12
#define PLACED_MOST_GROUPS 1024

// A draw whose first instance is at least this draws placed plants' parts: its instances count through the placed
// entries from here (instances.glsl, drawnObject), not through the draw list.
#define PLACED_INSTANCE_BASE 0x40000000u

// The passes' own set: the planting's eight (planting_records_shared.h, read here), then the parts, the kinds, the
// blocks, the views, the groups, each item's counts by group (made offsets by the scan), each view's groups' totals and
// bases, the commands (a view's groups each), the entries and the items (a block a view).
#define PLACED_PARTS_BINDING 8
#define PLACED_KINDS_BINDING 9
#define PLACED_BLOCKS_BINDING 10
#define PLACED_VIEW_BINDING 11
#define PLACED_GROUPS_BINDING 12
#define PLACED_BLOCK_COUNTS_BINDING 13
#define PLACED_TOTALS_BINDING 14
#define PLACED_COMMANDS_BINDING 15
#define PLACED_ENTRIES_BINDING 16
#define PLACED_ITEMS_BINDING 17
#define PLACED_BINDINGS 18

// The scene set's (render/vulkan/scene_layout.h): the slots' plants, each part's instance in the plant's frame, every
// view's placed entries.
#define SCENE_PLACED_PLANTS_BINDING 43
#define SCENE_PLACED_PARTS_BINDING 44
#define SCENE_PLACED_ENTRIES_BINDING 45

#endif
