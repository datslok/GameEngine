#ifndef RENDER_GPU_MESH_CLUSTER_CULLING_SHARED_H
#define RENDER_GPU_MESH_CLUSTER_CULLING_SHARED_H

// Mesh clusters culled on the GPU (render/gpu/mesh_cluster_culling.h), shared with mesh_cluster_culling.glsl and the
// mesh_cluster_*.comp passes. Only #defines of plain numbers, since GLSL reads them too.

// Threads a workgroup, one (instance, cluster) pair each: a block of the prefix sum. Each style's pairs start on a
// block's first (a run of the list starts where its first block's survivors do).
#define MESH_CLUSTER_GROUP_SIZE 256

// The ways of drawing, as the camera's culling has them (CAMERA_CULL_STYLES).
#define MESH_CLUSTER_STYLES 4

// GpuMeshCluster::counts: the vertices in the low byte, the triangles from this bit.
#define MESH_CLUSTER_TRIANGLE_SHIFT 8u

// The totals the passes write, a list's: pairs tested, survivors (before the room's limit), the style runs' firsts and
// counts (within the room, from the list's first), the triangles of the survivors written (the write pass adds them),
// the list's first place in the items (its number times the room), and the pairs the early list's occlusion test hid
// (the late list's candidates; the write pass adds them).
#define MESH_CLUSTER_TOTAL_TESTED 0
#define MESH_CLUSTER_TOTAL_KEPT 1
#define MESH_CLUSTER_TOTAL_RUN_FIRST 2
#define MESH_CLUSTER_TOTAL_RUN_COUNT 6
#define MESH_CLUSTER_TOTAL_TRIANGLES 10
#define MESH_CLUSTER_TOTAL_LIST_FIRST 11
#define MESH_CLUSTER_TOTAL_CANDIDATES 12
#define MESH_CLUSTER_TOTAL_WORDS 13

// Two-pass occlusion (stage 1d): the early list (tested against last frame's depth pyramid) and the late list (the
// pairs it hid, tested again against this frame's pyramid after the early list's depth is drawn). Each its own run of
// the items (room apart), totals (MESH_CLUSTER_TOTAL_WORDS apart) and task commands.
#define MESH_CLUSTER_EARLY 0
#define MESH_CLUSTER_LATE 1
#define MESH_CLUSTER_LISTS 2

// What the view makes of a cluster (meshClusterVisibility): hidden (out of the frustum, or facing away), seen whole (its
// draw is not rigid, so its sphere does not hold: never tested for occlusion), or seen with its sphere holding.
#define MESH_CLUSTER_HIDDEN 0u
#define MESH_CLUSTER_SEEN 1u
#define MESH_CLUSTER_SPHERE 2u

// GpuMeshClusterOcclusion::pyramid's w: occlusion is tested; the pyramid holds last frame's depth, drawn from previous.
#define MESH_CLUSTER_OCCLUSION_ON 1u
#define MESH_CLUSTER_OCCLUSION_HISTORY 2u

// A box corner nearer the camera's plane than this (clip w) cannot be projected: the cluster is never hidden.
#define MESH_CLUSTER_SMALLEST_W 0.001

// The work: the styles' first pairs (each a whole block), then their end, then the pairs that are real.
#define MESH_CLUSTER_WORK_STYLE_FIRST 0
#define MESH_CLUSTER_WORK_END 4
#define MESH_CLUSTER_WORK_REAL 5
#define MESH_CLUSTER_WORK_WORDS 8

// A style's survivors drawn as mesh shader workgroups this many across, then rows of them (a dimension is only sure to
// take 65535): meshClusterTaskGroups, meshClusterTaskIndex.
#define MESH_CLUSTER_TASK_WIDTH 32768u

// The mesh shader's workgroup: one warp on NVIDIA's GPUs, each thread placing up to two vertices and four triangles.
#define MESH_CLUSTER_MESH_THREADS 32

// The mesh shaders' push constants (the style drawn, then the list) sit after the vertex shaders' (GpuShadowView, GpuCaptureView).
#define MESH_CLUSTER_PUSH_OFFSET 112
#define MESH_CLUSTER_PUSH_BYTES 16

#endif
