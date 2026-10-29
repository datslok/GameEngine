#ifndef RENDER_GPU_CLOTH_GPU_SHARED_H
#define RENDER_GPU_CLOTH_GPU_SHARED_H

// Cloth on the GPU (render/gpu/cloth_gpu.h, cloth.glsl and the cloth_*.comp passes). Only #defines of plain numbers,
// since GLSL includes this file too; C++ reads them as typed constants in render/gpu/cloth_gpu.h.

// Invocations per workgroup in every cloth pass: one point or one constraint each. 64 fills both NVIDIA's 32-wide and
// AMD's 64-wide units.
#define CLOTH_GROUP_SIZE 64

// Invocations that sum one point's terms together in vertex block descent's sweep and the levels' gradient (a point
// has about 26: its inertia, about 6 edges, about 18 hinge corners, the hand), each taking every eighth and adding up
// with the others by subgroup shuffles. A power of two that divides CLOTH_GROUP_SIZE and every subgroup size used.
#define CLOTH_POINT_LANES 8

// A collider's kind (GpuCollider::kind), in the order of the Shape variant's types.
#define CLOTH_COLLIDER_SPHERE 0
#define CLOTH_COLLIDER_CAPSULE 1
#define CLOTH_COLLIDER_BOX 2

// The bending model (GpuClothTick::bendingModel), BendingModel's values.
#define CLOTH_BEND_ISOMETRIC 0
#define CLOTH_BEND_HINGE 1

// The solver (GpuClothTick::method), ClothMethod's values.
#define CLOTH_METHOD_XPBD 0
#define CLOTH_METHOD_BLOCK_DESCENT 1

// A point no pin moves (pinSlots), a tick with no hand (GpuClothTick::grabParticle), and a point in no stitch (stitchPartners).
#define CLOTH_NO_PIN 4294967295u
#define CLOTH_NO_GRAB 4294967295u
#define CLOTH_NO_STITCH 4294967295u

// Self-collision on the GPU. Room for this many pairs per point and per edge (the dress, resting, has about one per
// point; a pile of layers several); pairs past it are not kept, and counted (cloth_self_points.comp).
#define CLOTH_SELF_PAIRS_PER_FEATURE 8

// The pair search's hash table: at least this many buckets per triangle and edge (each item's box covers a few cells),
// a power of two. An item whose box spans more than CLOTH_SELF_CELLS_PER_AXIS cells along an axis goes on a list every
// query reads instead: cells are an edge and two reaches wide, so a triangle's diagonal straddling a cell's side takes
// three, and only a cloth stretched or torn wide open takes more. A query looks at most CLOTH_SELF_QUERY_CELLS_PER_AXIS
// cells an axis.
#define CLOTH_SELF_BUCKETS_PER_ITEM 8
#define CLOTH_SELF_CELLS_PER_AXIS 3
#define CLOTH_SELF_QUERY_CELLS_PER_AXIS 4

// The pair search's modes, in the hash and scan passes' level push constant.
#define CLOTH_SELF_COUNT 0
#define CLOTH_SELF_SCATTER 1
#define CLOTH_SELF_SCAN_BLOCKS 0
#define CLOTH_SELF_SCAN_TOTALS 1
#define CLOTH_SELF_SCAN_ADD 2

// pairState's words (cloth_self_pass.glsl): how many pairs were found, how many did not fit, the large items' count,
// then the pairs' dispatch sizes (x, y, z) for vkCmdDispatchIndirect.
#define CLOTH_SELF_PAIR_COUNT 0
#define CLOTH_SELF_PAIRS_LOST 1
#define CLOTH_SELF_LARGE_COUNT 2
#define CLOTH_SELF_DISPATCH 4
#define CLOTH_SELF_STATE_WORDS 8

#endif
