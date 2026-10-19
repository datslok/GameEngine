#ifndef RENDER_GPU_DEPTH_RANGE_SHARED_H
#define RENDER_GPU_DEPTH_RANGE_SHARED_H

// The depth reduction that fits the sun's cascades to the screen (depth_range.glsl, depth_range.comp). Only #defines of
// plain numbers, since GLSL includes this file too; C++ reads them as typed constants in render/gpu/depth_range.h.

// Each workgroup is this many invocations square (256 in all), each reading a square of DEPTH_RANGE_TEXELS_PER_SIDE
// pixels on a side, so a group covers 32 x 32 pixels: 3,600 groups at 2560x1440.
#define DEPTH_RANGE_GROUP_SIDE 16
#define DEPTH_RANGE_TEXELS_PER_SIDE 2

// The farthest end of a range with no surface in it: larger than any depth's bits, so the first surface replaces it.
#define DEPTH_RANGE_NO_SURFACE_FARTHEST 4294967295u

#endif
