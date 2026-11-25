#ifndef RENDER_GPU_VIRTUAL_SHADOWS_SHARED_H
#define RENDER_GPU_VIRTUAL_SHADOWS_SHARED_H

// The sun's virtual shadow maps (virtual_shadows.glsl). Only #defines of plain numbers, since GLSL includes this file
// too; C++ reads them as typed constants in render/gpu/virtual_shadows.h.

// A page's side in texels (Unreal's, Frogfood's, StratusGFX's): 64 KB of 32-bit depth.
#define VIRTUAL_SHADOW_PAGE_TEXELS 128

// The most levels a clipmap can have: from 0.5 m, the 16th reaches 16 km.
#define VIRTUAL_SHADOW_MAX_LEVELS 16

// How many pages a point is kept from its level's edge (the level choice's margin): the level slides by whole pages, so
// the camera sits up to half a page off its centre, and the other half page (64 texels) holds a soft shadow's rays
// (VIRTUAL_SHADOW_RAY_TEXELS) inside the level.
#define VIRTUAL_SHADOW_MARGIN_PAGES 1

// The level of a point no level holds: past the last one, where the far shadow map answers.
#define VIRTUAL_SHADOW_NO_LEVEL -1

// How far across a soft shadow's rays reach from their point, in its level's texels (virtual_shadow_rays.glsl): the
// cascades' widest penumbra (MAX_SHADOW_FILTER_TEXELS), as Unreal clamps its penumbra, so a point's samples all lie on
// the pages it marked.
#define VIRTUAL_SHADOW_RAY_TEXELS 12

// How far round a pixel's texel the pages it may read are marked: its rays' reach. Under half the margin's page, so a
// pixel marks at most the four pages round a corner.
#define VIRTUAL_SHADOW_FILTER_TEXELS VIRTUAL_SHADOW_RAY_TEXELS

// A page table entry: flags in the top bits, the physical page (its place in the pool) in the low 16.
#define VIRTUAL_SHADOW_ENTRY_ALLOCATED 0x80000000u
#define VIRTUAL_SHADOW_ENTRY_REQUESTED 0x40000000u
#define VIRTUAL_SHADOW_ENTRY_DIRTY 0x20000000u
#define VIRTUAL_SHADOW_ENTRY_FAILED 0x10000000u
#define VIRTUAL_SHADOW_ENTRY_FLAGS 0xF0000000u
#define VIRTUAL_SHADOW_ENTRY_PHYSICAL 0x0000FFFFu

// Where the allocating pass finds the k-th needed entry a place (virtualShadowAllocationSource).
#define VIRTUAL_SHADOW_FROM_FREE 0
#define VIRTUAL_SHADOW_FROM_CACHED 1
#define VIRTUAL_SHADOW_FROM_NOWHERE 2

// The pool's most pages: 16 bits of an entry, and 4096 is Unreal's default; 16384 pages of 128 x 128 x 4 B are 1 GB.
#define VIRTUAL_SHADOW_MAX_POOL_PAGES 16384

// The passes' workgroups: marking a square of pixels, resolving a row of entries, one group allocating.
#define VIRTUAL_SHADOW_MARK_GROUP_SIDE 8
#define VIRTUAL_SHADOW_RESOLVE_GROUP 64
#define VIRTUAL_SHADOW_ALLOCATE_GROUP 256

// The pages' counters (uints): the free list's top lasts from frame to frame, the rest start at 0 every frame.
#define VIRTUAL_SHADOW_COUNT_FREE 0
#define VIRTUAL_SHADOW_COUNT_NEEDED 1
#define VIRTUAL_SHADOW_COUNT_CACHED 2
#define VIRTUAL_SHADOW_COUNT_REQUESTED 3
#define VIRTUAL_SHADOW_COUNT_KEPT 4
#define VIRTUAL_SHADOW_COUNT_ALLOCATED 5
#define VIRTUAL_SHADOW_COUNT_EVICTED 6
#define VIRTUAL_SHADOW_COUNT_FAILED 7
#define VIRTUAL_SHADOW_COUNT_STALE 8
#define VIRTUAL_SHADOW_COUNTERS 12

// Set 1's bindings the main pass reads the pages through (render/vulkan/scene_layout.h): the record, the table, the
// pool's depth (4c-ii); the mesh shaders drawing into the pages read the first two. The atomics' fragment shaders
// (4c-iv) write the pool's whole numbers through the last (a stand-in while the pool is a depth image).
#define SCENE_VIRTUAL_SHADOW_FRAME_BINDING 59
#define SCENE_VIRTUAL_SHADOW_TABLE_BINDING 60
#define SCENE_VIRTUAL_SHADOW_POOL_BINDING 61
#define SCENE_VIRTUAL_SHADOW_ATOMIC_POOL_BINDING 62

// Drawing into the pages (stage 4c-ii). A page pair is two words: the caster's place in its clusters' list, then its
// level (the top 4 bits: VIRTUAL_SHADOW_MAX_LEVELS 16), its page across (6 bits each: 64 pages a side at most) and its
// physical page (the low 16, as an entry's).
#define VIRTUAL_SHADOW_PAIR_LEVEL_SHIFT 28
#define VIRTUAL_SHADOW_PAIR_PAGE_Y_SHIFT 22
#define VIRTUAL_SHADOW_PAIR_PAGE_X_SHIFT 16
#define VIRTUAL_SHADOW_PAIR_PAGE_MASK 0x3Fu
#define VIRTUAL_SHADOW_PAIR_LEVEL_MASK 0xFu

// The pairs' two lists: casters drawn without a fragment shader, and cut-outs (their holes thrown away).
#define VIRTUAL_SHADOW_PAIRS_PLAIN 0
#define VIRTUAL_SHADOW_PAIRS_CUT_OUT 1
#define VIRTUAL_SHADOW_PAIR_LISTS 2

// The pairs' buffer: a header, then each list's room of pairs. The header: each list's pairs found (words 0 and 1, past
// the room too), the pairs' triangles (2), each list's mesh shader draw (VkDrawMeshTasksIndirectCommandEXT, three words
// each from 4); then the costs, counted only while VirtualShadowSettings::countCosts (4c-v).
#define VIRTUAL_SHADOW_HEADER_PAIRS 0
#define VIRTUAL_SHADOW_HEADER_TRIANGLES 2
#define VIRTUAL_SHADOW_HEADER_TASKS 4
#define VIRTUAL_SHADOW_HEADER_WORDS 96

// The costs: from word 16 three words a level (the clusters reaching it, their pairs, the pairs' triangles, all found
// past the room too); from 64 the casters' words: those seen by the camera (clusters, their triangles), those out of its
// frustum (clusters, triangles, their pairs, the pairs' triangles), those kept whole (clusters, pairs, the pairs' triangles),
// and of those, the ones kept whole for swaying without bones (clusters, pairs, the pairs' triangles; 4c-vi).
#define VIRTUAL_SHADOW_HEADER_LEVELS 16
#define VIRTUAL_SHADOW_LEVEL_WORDS 3
#define VIRTUAL_SHADOW_LEVEL_CLUSTERS 0
#define VIRTUAL_SHADOW_LEVEL_PAIRS 1
#define VIRTUAL_SHADOW_LEVEL_TRIANGLES 2
#define VIRTUAL_SHADOW_HEADER_CASTERS 64
#define VIRTUAL_SHADOW_CASTER_SEEN 0
#define VIRTUAL_SHADOW_CASTER_UNSEEN 2
#define VIRTUAL_SHADOW_CASTER_WHOLE 6
#define VIRTUAL_SHADOW_CASTER_SWAYING 9
#define VIRTUAL_SHADOW_CASTER_WORDS 12

// From 80, the draws kept whole for their shape, by its first kind (clusters, pairs, the pairs' triangles each): a terrain
// patch, a liana, an impostor's card, following the ground, cards thinned in this view.
#define VIRTUAL_SHADOW_HEADER_SHAPES 80
#define VIRTUAL_SHADOW_SHAPE_TERRAIN 0
#define VIRTUAL_SHADOW_SHAPE_LIANA 1
#define VIRTUAL_SHADOW_SHAPE_CARD 2
#define VIRTUAL_SHADOW_SHAPE_GROUND 3
#define VIRTUAL_SHADOW_SHAPE_THINNED 4
#define VIRTUAL_SHADOW_SHAPES 5

// The expanding pass: a workgroup a surviving cluster, a thread a level.
#define VIRTUAL_SHADOW_EXPAND_GROUP 16

// The pairs' room at first (2 MB); a frame finding more grows it to the next power of two for the frames after.
#define VIRTUAL_SHADOW_FIRST_PAIR_ROOM 262144

#endif
