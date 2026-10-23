#ifndef RENDER_GPU_MIRRORED_LIGHT_SHARED_H
#define RENDER_GPU_MIRRORED_LIGHT_SHARED_H

// Mirrored lights' numbers, shared by render/gpu/mirrored_light.h and the shaders (mirrored_light.glsl,
// mirrored_light.comp). Only #defines of plain numbers, since GLSL reads them too.

// The most pairs of a light and a shiny surface one frame lights through: the pass record holds this many.
#define MIRROR_MAX_PAIRS 16

// The search for the point of reflection: at most this many Newton steps after the first (flat mirror) step, each
// halved at most this many times while its point falls off the surface; done once the half vector's tilt off the
// surface's normal (the constraint's length, the sum of two unit vectors' tangential parts) is below the tolerance.
#define MIRROR_MAX_STEPS 8
#define MIRROR_BACKTRACKS 4
#define MIRROR_SOLVE_TOLERANCE 0.001

// A surface curving less than this (per metre: a radius of over a kilometre) counts as flat, where the first step is
// exact: a point of reflection it puts off the mirror is off it (where only a rough mirror's footprint gives light),
// with no Newton steps to creep to its edge.
#define MIRROR_FLAT_CURVATURE 0.001

// On a flat mirror, the flat step's point counts as on it only where the surface there faces the same way (normals
// closer than this cosine, about 2.5 degrees): landing on a thin mirror's rim is landing off the mirror.
#define MIRROR_SAME_PLANE_COSINE 0.999

// Rays leave the lit point (towards the camera) and the mirror (along its normal) this far, plus this much per metre
// from the camera, so they do not meet the surface they start on.
#define MIRROR_RAY_OFFSET 0.005
#define MIRROR_RAY_OFFSET_PER_METRE 0.001

// Surfaces rougher than this reflect a light as a footprint (rough mirrors: mirrorFootprint): up to it the footprint is
// under about a centimetre across at a metre, too small to see, and the one point of reflection costs one shadow ray
// rather than a mirror ray and a shadow ray per sample.
#define MIRROR_FOOTPRINT_ROUGHNESS 0.1

// Past the setting's roughness, the mirrored light fades out over this much more, so no surface pops in or out.
#define MIRROR_ROUGHNESS_FADE 0.05

// The footprint's samples each frame: per pixel of the mirrored light pass (temporal anti-aliasing averages the frames),
// and per probe ray's hit, reflection probe texel and traced reflection's hit (their many rays and filters average them).
#define MIRROR_FOOTPRINT_SAMPLES 4
#define MIRROR_HIT_FOOTPRINT_SAMPLES 1

// The pass's workgroups: 8 x 8 pixels.
#define MIRROR_GROUP_SIDE 8

// The pair's light kinds in the pass record.
#define MIRROR_POINT_LIGHT 0
#define MIRROR_SPOT_LIGHT 1

#endif
