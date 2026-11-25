#ifndef RENDER_GPU_CAMERA_CULLING_SHARED_H
#define RENDER_GPU_CAMERA_CULLING_SHARED_H

// The camera's GPU culling (render/gpu/camera_culling.h), shared with camera_culling.glsl and the camera_cull_*.comp
// passes. Only #defines of plain numbers, since GLSL reads them too.

// Threads a workgroup, one candidate each: a block of the prefix sum.
#define CAMERA_CULL_GROUP_SIZE 256

// A candidate's flags (cameraCandidateFlags): the camera draws it with this slot's mesh, the velocity pass too, thinned,
// fading (its entry's fade field, cameraCandidateFade, worked out again by the write pass).
#define CAMERA_CULL_DRAWN 1u
#define CAMERA_CULL_MOVING 2u
#define CAMERA_CULL_THINNED 4u
#define CAMERA_CULL_FADES 8u

// The flags pass keeps a visible candidate's place in the candidate list above its flags' bits.
#define CAMERA_CULL_FLAG_BITS 4u

// GpuCameraCull::flags: the velocity pass's list is built (temporal anti-aliasing on; for a shadow tile, the casters
// that moved in the second list, kept apart from the still ones); things sway this frame; a shadow tile (shadowTakes);
// mesh clusters swaying unbound culled by their spheres grown by the sway's reach (meshClusterSwayingPose); lianas'
// clusters culled by their spans' bounds as bent (meshClusterLianaPose); terrain patches' clusters culled by their own
// boxes on the land (meshClusterTerrainPose).
#define CAMERA_CULL_MOTION 1u
#define CAMERA_CULL_SWAY 2u
#define CAMERA_CULL_SHADOW 4u
#define CAMERA_CULL_SWAY_REACH 8u
#define CAMERA_CULL_LIANA_BOUNDS 16u
#define CAMERA_CULL_TERRAIN_BOUNDS 32u

// The packed commands' counts (camera_cull_compact.comp): the camera's four styles, then the velocity pass's.
#define CAMERA_CULL_STYLES 4
#define CAMERA_CULL_DRAW_COUNTS 8

// GpuCameraCullCounts: the draws drawn at each level slot, then the camera's and the velocity pass's totals.
#define CAMERA_CULL_COUNT_DRAWN 4
#define CAMERA_CULL_COUNT_MOVING 5
#define CAMERA_CULL_COUNTS 8

// Views share the flags, offsets and block sums in batches of this many (the dispatch's second dimension), since those
// are spent once the batch's lists are written; commands and lists are each view's own.
#define CAMERA_CULL_SCRATCH_VIEWS 16

// GpuCameraCull::slices: where the view's part of each shared working buffer starts (flags and offsets by candidate,
// block sums with the totals after them: its place in its batch; commands and packed commands by group, packed places
// with the total after: its own).
#define CAMERA_CULL_SLICE_FLAGS 0
#define CAMERA_CULL_SLICE_BLOCKS 1
#define CAMERA_CULL_SLICE_COMMANDS 2
#define CAMERA_CULL_SLICE_PLACES 3

// GpuCameraCull::segmentSlices (GPU-driven drawing's stage 7b): where the view's part starts of the segments' offsets
// and visible list (by segment), and of their block sums (the totals after them), by its place in its batch.
#define CAMERA_CULL_SEGMENT_SLICE_OFFSETS 0
#define CAMERA_CULL_SEGMENT_SLICE_BLOCKS 1

// Words of the indirect dispatches a batch of views takes: the flags pass's (x y z), then the write pass's.
#define CAMERA_CULL_DISPATCH_WORDS 6

#endif
