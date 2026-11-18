#ifndef RENDER_GPU_CAMERA_CULLING_SHARED_H
#define RENDER_GPU_CAMERA_CULLING_SHARED_H

// The camera's GPU culling (render/gpu/camera_culling.h), shared with camera_culling.glsl and the camera_cull_*.comp
// passes. Only #defines of plain numbers, since GLSL reads them too.

// Threads a workgroup, one candidate each: a block of the prefix sum.
#define CAMERA_CULL_GROUP_SIZE 256

// A candidate's flags (cameraCandidateFlags): the camera draws it with this slot's mesh, the velocity pass too, thinned.
#define CAMERA_CULL_DRAWN 1u
#define CAMERA_CULL_MOVING 2u
#define CAMERA_CULL_THINNED 4u

// GpuCameraCull::flags: the velocity pass's list is built (temporal anti-aliasing on); things sway this frame.
#define CAMERA_CULL_MOTION 1u
#define CAMERA_CULL_SWAY 2u

// The packed commands' counts (camera_cull_compact.comp): the camera's four styles, then the velocity pass's.
#define CAMERA_CULL_STYLES 4
#define CAMERA_CULL_DRAW_COUNTS 8

// GpuCameraCullCounts: the draws drawn at each level slot, then the camera's and the velocity pass's totals.
#define CAMERA_CULL_COUNT_DRAWN 4
#define CAMERA_CULL_COUNT_MOVING 5
#define CAMERA_CULL_COUNTS 8

// The views culled in one dispatch (the camera, six probe faces, a mirror): the dispatch's second dimension.
#define CAMERA_CULL_MAX_VIEWS 8

// GpuCameraCull::slices: where the view's part of each shared working buffer starts (flags and offsets by candidate,
// block sums with the totals after them, commands and packed commands by group, packed places with the total after).
#define CAMERA_CULL_SLICE_FLAGS 0
#define CAMERA_CULL_SLICE_BLOCKS 1
#define CAMERA_CULL_SLICE_COMMANDS 2
#define CAMERA_CULL_SLICE_PLACES 3

#endif
