#ifndef RENDER_GPU_SCREEN_REFLECTIONS_SHARED_H
#define RENDER_GPU_SCREEN_REFLECTIONS_SHARED_H

// Screen-space reflections' numbers, shared by render/gpu/screen_reflections.h and the shaders (screen_reflections.glsl,
// reflections.comp, depth_pyramid.comp and colour_pyramid.comp). Only #defines of plain numbers, since
// GLSL reads them too.

// The passes' workgroups: a square of this many pixels a side, one pixel an invocation.
#define SSR_GROUP_SIDE 8

// The depth pyramid's most levels: enough to reach one texel from 32768 pixels across.
#define SSR_MAX_PYRAMID_LEVELS 16

// How far past a cell's edge, in texels, a step out of the cell lands, so it is inside the next cell and never stuck
// on the edge between them.
#define SSR_CELL_CROSSING 0.01

// A ray that leaves the near side of the camera is cut where it is this far in front of it (view distance, metres),
// before the depth range cuts it at the near plane, so its projection never divides by nothing.
#define SSR_NEAREST_W 0.001

// The ray starts this many pixels' footprints off the surface, along its normal, so the surface it leaves does not
// hide it (the depth image holds one depth per pixel, a little nearer or farther than the surface across the pixel).
#define SSR_START_OFFSET_PIXELS 2.0

// The reflected cone's half angle: its tangent per unit of GGX width (alpha, roughness squared). GGX's lobe falls to half
// its peak at a halfway vector tan = 0.64 alpha off the normal, and the mirror turns twice as far.
#define SSR_CONE_SPREAD 1.29

// Hits fade out over the last fifth of the reach.
#define SSR_DISTANCE_FADE_START 0.8

// Hits on a surface the ray meets from behind (its normal along the ray) are not real: the picture shows that
// surface's other side. Full trust from this cosine against the ray down to head-on, none at grazing and behind.
#define SSR_FRONT_FACE_COSINE 0.2

#endif
