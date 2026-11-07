#ifndef RENDER_GPU_PLANAR_REFLECTION_SHARED_H
#define RENDER_GPU_PLANAR_REFLECTION_SHARED_H

// Planar reflections' numbers, shared by render/gpu/planar_reflection.h and planar_reflection.glsl. Only #defines of
// plain numbers, since GLSL reads them too.

// The most mirror planes a frame draws: the settings' limit.
#define PLANAR_MAX_MIRRORS 1

// The mirrored view is cut this far in front of the mirror (metres), so the mirror's own face, exactly on the plane, does
// not flicker in and out of its own reflection.
#define PLANAR_CLIP_LIFT 0.002

// How far from the plane a pixel may be and still be on the mirror: this much (metres) plus this share of its distance
// from the camera, since depth's rounding grows with distance.
#define PLANAR_PLANE_TOLERANCE 0.01
#define PLANAR_PLANE_TOLERANCE_PER_METRE 0.005

// The least cosine between a pixel's normal and the mirror's for it to be on the mirror.
#define PLANAR_NORMAL_COSINE 0.98

#endif
