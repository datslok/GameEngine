#ifndef RENDER_GPU_AMBIENT_OCCLUSION_SHARED_H
#define RENDER_GPU_AMBIENT_OCCLUSION_SHARED_H

// Ambient occlusion's numbers, shared by render/gpu/ambient_occlusion.h and the shaders (ambient_occlusion.glsl,
// ambient_occlusion.comp, ambient_occlusion_blur.comp). Only #defines of plain numbers, since GLSL reads them too.

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

// The blur while there is no anti-aliasing to average the noise: this many texels each way, a neighbour counting less
// the further its view distance is from the pixel's, nothing past this share of it.
#define AO_BLUR_RADIUS 2
#define AO_BLUR_DEPTH_SHARE 0.1

// The pass's noise (aoNoise): the slices' turn is interleaved gradient noise moved this many pixels along both axes a
// frame (Jimenez's constant, which steps it evenly), and the steps' stagger starts from the noise at the pixel moved by
// this offset and goes on by the golden ratio's fraction a frame, a sequence of its own, so that over the frames every
// slice angle meets every step stagger.
#define AO_NOISE_FRAME_SHIFT 5.588238
#define AO_STEP_NOISE_OFFSET_X 23.0
#define AO_STEP_NOISE_OFFSET_Y 41.0
#define AO_STEP_NOISE_FRAME_STEP 0.6180339887

// The most frames the temporal accumulation's average may hold (AmbientOcclusionSettings::historyFrames' limit).
#define AO_MAX_HISTORY_FRAMES 64

// The passes' workgroups: 8 x 8 pixels.
#define AO_GROUP_SIDE 8

#endif
