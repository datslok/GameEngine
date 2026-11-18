// Levels cross-faded by a dither (docs/research/LOD.md, Hiding the switches), included with #include "level_fade.glsl":
// mirrors render/gpu/level_fade.h. Pure functions; the pixel and the frame are arguments.
#ifndef LEVEL_FADE_GLSL
#define LEVEL_FADE_GLSL

#include "render/gpu/instance_shared.h"
#include "dither.glsl"

// A draw list entry's fade field (0: not fading).
uint drawListFade(uint entry) {
    return (entry & DRAW_LIST_FADE_MASK) >> DRAW_LIST_FADE_SHIFT;
}

// The field of a level drawn while its draw fades: share 0 to 1 across the band, coarse for the coarser level. Never 0.
uint levelFadeField(float share, bool coarse) {
    uint step = min(uint(max(share, 0.0) * float(DRAW_LIST_FADE_STEPS)), DRAW_LIST_FADE_STEPS - 1u);
    return (coarse ? DRAW_LIST_FADE_COARSE : 0u) | (step + 1u);
}

// The noise below which the coarser level keeps a pixel: the step's middle, so neither level ever keeps all or none.
float levelFadeThreshold(uint field) {
    return (float((field & ~DRAW_LIST_FADE_COARSE) - 1u) + 0.5) / float(DRAW_LIST_FADE_STEPS);
}

// Whether the level keeps a pixel of noise 0..1: the coarser below the threshold, the finer from it, so the two levels
// of one draw split every pixel between them; a level not fading keeps them all.
bool levelFadeKeeps(uint field, float noise) {
    if (field == 0u) {
        return true;
    }

    bool below = noise < levelFadeThreshold(field);
    return (field & DRAW_LIST_FADE_COARSE) != 0u ? below : !below;
}

// The pixel's noise for the fade, turned each frame (frame below 1024) so temporal anti-aliasing and the upscalers
// average the dither into a blend; apart from the sun cascades' fixed dither.
float levelFadeNoise(vec2 pixel, uint frame) {
    return interleavedGradientNoise(pixel + vec2(47.0, 17.0) + 5.588238 * float(frame));
}

#endif
