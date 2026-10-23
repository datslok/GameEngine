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
// exact: a point of reflection it puts off the mirror means there is none, with no Newton steps to creep to its edge.
#define MIRROR_FLAT_CURVATURE 0.001

// Rays leave the lit point (towards the camera) and the mirror (along its normal) this far, plus this much per metre
// from the camera, so they do not meet the surface they start on.
#define MIRROR_RAY_OFFSET 0.005
#define MIRROR_RAY_OFFSET_PER_METRE 0.001

// The pass's workgroups: 8 x 8 pixels.
#define MIRROR_GROUP_SIDE 8

// The pair's light kinds in the pass record.
#define MIRROR_POINT_LIGHT 0
#define MIRROR_SPOT_LIGHT 1

#endif
