#ifndef RENDER_GPU_TAA_SHARED_H
#define RENDER_GPU_TAA_SHARED_H

// Temporal anti-aliasing's numbers, shared by render/gpu/taa.h and the shaders (taa.glsl, taa.comp, motion.vert and
// motion.frag). Only #defines of plain numbers, since GLSL reads them too.

// The jitter's cycle: this many frames of Halton (2, 3) points, then it starts again.
#define TAA_JITTER_FRAMES 16

// The velocity image's clear value: no moving object was drawn at this pixel, so its motion is the camera's alone,
// worked out from its depth. Far beyond any real motion (two screens' width in NDC is 4); a pixel's motion is "none"
// above the test value.
#define TAA_NO_MOTION 1000.0
#define TAA_NO_MOTION_TEST 500.0

// Upscaling's reconstruction: a sample d output pixels from a pixel's centre weighs exp(-falloff d^2), Karis's
// Gaussian fit of the Blackman-Harris window (Karis 2014, "High quality temporal supersampling").
#define TAA_SAMPLE_FALLOFF 2.29

// The resolve pass's workgroups: a square of this many pixels a side, one pixel an invocation.
#define TAA_GROUP_SIDE 8

// The motion debug view: a pixel's motion in pixels a frame, times this, as red (sideways) and green (up and down).
#define TAA_MOTION_VIEW_SCALE 0.1

#endif
