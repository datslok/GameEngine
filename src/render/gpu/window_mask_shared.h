#ifndef RENDER_GPU_WINDOW_MASK_SHARED_H
#define RENDER_GPU_WINDOW_MASK_SHARED_H

/*
* The window mask's numbers (render/gpu/window_mask.h), shared by render/gpu/window_mask.cpp and window_mask.glsl, so
* they cannot drift apart. GLSL reads this file too, so it may only hold #defines of plain numbers.
*/

// The height of a row of windows for their random numbers (metres): rows a metre apart or more light on their own.
#define WINDOW_ROW_HEIGHT 1.0

// The far blend: windows keep their own lit or dark state while a pixel spans under WINDOW_FAR_START cells, and are
// their average from WINDOW_FAR_FULL cells, so a distant band glows steadily instead of flickering.
#define WINDOW_FAR_START 0.5
#define WINDOW_FAR_FULL 1.5

// A face's two facet numbers (0..1) rounded to this many steps each for its seed: robust to the tiny differences
// interpolating three equal corner values can leave.
#define WINDOW_SEED_STEPS 1024.0

// A lit window's brightness varies from WINDOW_LEAST_BRIGHTNESS to that plus WINDOW_BRIGHTNESS_RANGE (mean 1).
#define WINDOW_LEAST_BRIGHTNESS 0.6
#define WINDOW_BRIGHTNESS_RANGE 0.8

#endif
