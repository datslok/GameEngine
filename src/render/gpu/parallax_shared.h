#ifndef RENDER_GPU_PARALLAX_SHARED_H
#define RENDER_GPU_PARALLAX_SHARED_H

/*
* Parallax occlusion mapping's numbers, shared by render/gpu/parallax.cpp (the tested mirror), parallax.glsl and the
* main pass's search, so they cannot drift apart. GLSL reads this file too, so it may only hold #defines of plain numbers.
*/

// Steps through the height map looking straight down; more towards grazing views, up to the setting's most
// (MaterialSettings::parallaxSteps), which may be at most PARALLAX_MOST_STEPS.
#define PARALLAX_FEWEST_STEPS 8
#define PARALLAX_MOST_STEPS 64

// The view's (or the light's) cosine to the surface is kept at least this, so a grazing ray's shift stays finite: at
// most ten times the depth sideways.
#define PARALLAX_SMALLEST_VIEW_COSINE 0.1

// The fade with distance, by the depth seen head-on in pixels: full from 2 pixels, none under half a pixel, where the
// height map's mips can no longer hold the bumps and the shift would only shimmer.
#define PARALLAX_FADE_FULL_PIXELS 2.0
#define PARALLAX_FADE_NONE_PIXELS 0.5

// Self-shadowing: steps from the hit up towards the light, and how sharply a bump poking above that ray darkens it (by
// how far it pokes over how far the ray has climbed: 4 shades fully where it pokes a quarter of the climb).
#define PARALLAX_SHADOW_STEPS 16
#define PARALLAX_SHADOW_SOFTNESS 4.0

#endif
