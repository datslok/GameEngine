#ifndef RENDER_GPU_MOTION_BLUR_SHARED_H
#define RENDER_GPU_MOTION_BLUR_SHARED_H

// Numbers motion blur (render/gpu/motion_blur.h) shares with GLSL (motion_blur.glsl and its passes). Only #defines of
// plain numbers, since GLSL reads them too.

// The tiles' side in output pixels, and the longest half-blur (pixels either way): a pixel's blur can then reach only
// into the tiles next to its own, which the neighbourhood's largest motion covers.
#define MOTION_BLUR_TILE 32

// Samples along the neighbourhood's motion besides the pixel itself (odd: the middle one is the pixel, left out).
#define MOTION_BLUR_SAMPLES 15

// Below this half-blur (pixels) a pixel counts as still: a tile neighbourhood no faster is copied as it is.
#define MOTION_BLUR_LEAST_MOTION 0.5

// How far behind another a point may lie, as a share of the other's view depth, and still count as the same surface.
#define MOTION_BLUR_DEPTH_SOFTNESS 0.02

// The view depth the sky counts as (metres): behind everything, and within half floats' range.
#define MOTION_BLUR_SKY_DEPTH 60000.0

// The passes' square workgroups: the tiles pass gives each tile MOTION_BLUR_GROUP x MOTION_BLUR_GROUP invocations, each
// preparing a square of MOTION_BLUR_TILE / MOTION_BLUR_GROUP pixels a side.
#define MOTION_BLUR_GROUP 8

#endif
