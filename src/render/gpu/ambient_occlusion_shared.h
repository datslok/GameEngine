#ifndef RENDER_GPU_AMBIENT_OCCLUSION_SHARED_H
#define RENDER_GPU_AMBIENT_OCCLUSION_SHARED_H

// Ambient occlusion's numbers, shared by render/gpu/ambient_occlusion.h and the shaders (ambient_occlusion.glsl,
// ambient_occlusion.comp, ambient_occlusion_temporal.comp, ambient_occlusion_denoise.comp). Only #defines of plain numbers, since GLSL reads them too.

// The most slices a pixel takes each frame, and steps along each side of a slice: the settings' limits.
#define AO_MAX_SLICES 8
#define AO_MAX_STEPS 16

// The sectors a slice's half circle is cut into, one bit each of a 32-bit mask.
#define AO_SECTORS 32

// Samples nearer the pixel than this (pixels) would read its own surface: the first step starts here. A footprint
// smaller than it on screen (a pixel far away, or a tiny radius) is left open.
#define AO_MIN_SAMPLE_PIXELS 1.5

// The longest walk on screen (pixels), so a surface right in front of the camera does not read the whole screen.
#define AO_MAX_SCREEN_RADIUS 256.0

// Jimenez et al. 2016's fit of light bouncing between a crease's walls (multiBounceOcclusion): the visibility v becomes
// max(v, ((a v + b) v + c) v), with a, b and c lines in the albedo: slope then offset.
#define AO_BOUNCE_A_SLOPE 2.0404
#define AO_BOUNCE_A_OFFSET -0.3324
#define AO_BOUNCE_B_SLOPE -4.7951
#define AO_BOUNCE_B_OFFSET 0.6417
#define AO_BOUNCE_C_SLOPE 2.7552
#define AO_BOUNCE_C_OFFSET 0.6903

// The denoiser (ambient_occlusion_denoise.comp, an edge-aware a-trous filter): the most passes (each twice as wide as
// the last, a 5 x 5 B3-spline kernel), the power of the normals' cosine a tap is weighted by (corners stop it), and how
// many first passes filter the visibility too (the light gathered takes them all: bounce light is soft, the darkening in
// creases is detail).
#define AO_DENOISE_MAX_PASSES 5
#define AO_DENOISE_NORMAL_POWER 32.0
#define AO_DENOISE_VISIBILITY_PASSES 1

// The pass's noise (aoNoise): a blue noise tile (core/blue_noise.h) this many texels a side, three channels, repeated
// over the screen, each channel moved on by an irrational step a frame (the golden ratio's fraction, sqrt(2) - 1,
// sqrt(3) - 1; with 1 independent over the rationals, so no two channels' sequences ever fall into step) and wrapped:
// blue across the screen, evenly spread over the frames.
#define AO_BLUE_NOISE_SIZE 64
#define AO_SLICE_NOISE_FRAME_STEP 0.6180339887
#define AO_STEP_NOISE_FRAME_STEP 0.4142135624
#define AO_SECTOR_NOISE_FRAME_STEP 0.7320508076

// The most frames the temporal accumulation's average may hold (AmbientOcclusionSettings::historyFrames' limit).
#define AO_MAX_HISTORY_FRAMES 64

// Anti-lag (aoLightChanged): the fast average's share of each frame (about 3 frames), the least threshold as a share of
// the slow mean plus a floor (in the picture's linear light), and the least age a history must have before it is tested
// (a younger one follows quickly anyway, and its noise estimate is still too young to trust).
#define AO_ANTILAG_FAST_SHARE 0.3333333
#define AO_ANTILAG_RELATIVE_FLOOR 0.05
#define AO_ANTILAG_ABSOLUTE_FLOOR 0.001
#define AO_ANTILAG_LEAST_AGE 4.0

// The passes' workgroups: 8 x 8 pixels.
#define AO_GROUP_SIDE 8

#endif
