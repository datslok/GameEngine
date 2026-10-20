#ifndef RENDER_GPU_ATMOSPHERE_SHARED_H
#define RENDER_GPU_ATMOSPHERE_SHARED_H

// The sky and atmosphere (render/gpu/atmosphere.h, atmosphere.glsl and the passes that include it; Hillaire 2020, "A
// Scalable and Production Ready Sky and Atmosphere Rendering Technique"). Only #defines of plain numbers, since GLSL
// includes this file too; C++ reads them as typed constants in render/gpu/atmosphere.h.

// The transmittance table: how much light gets from any height, in any direction, out to space. Its size, and the steps
// each texel's ray is cut into (the paper's 256 x 64 and 40).
#define ATMOSPHERE_TRANSMITTANCE_WIDTH 256
#define ATMOSPHERE_TRANSMITTANCE_HEIGHT 64
#define ATMOSPHERE_TRANSMITTANCE_STEPS 40

// The multiple scattering table: light scattered twice or more, per height and sun angle. Its side, and each texel's
// directions (one invocation each) and steps along each (the paper's 32 x 32, 64 and 20).
#define ATMOSPHERE_MULTIPLE_SCATTERING_SIZE 32
#define ATMOSPHERE_MULTIPLE_SCATTERING_DIRECTIONS 64
#define ATMOSPHERE_MULTIPLE_SCATTERING_STEPS 20

// The sky view table: the sky's light in every direction round the camera, made each frame. Its size and steps (the
// paper's 192 x 108 and up to 30).
#define ATMOSPHERE_SKY_VIEW_WIDTH 192
#define ATMOSPHERE_SKY_VIEW_HEIGHT 108
#define ATMOSPHERE_SKY_VIEW_STEPS 30

// Where in each step the medium is sampled, as a share of the step: the midpoint. The paper's 0.3 leans towards each
// step's denser start: straight up it overestimated blue's optical depth by 2.5% (the light kept 0.9% low against the
// closed form); the midpoint is within 0.05%.
#define ATMOSPHERE_SAMPLE_OFFSET 0.5

// Directions the flat ambient light averages the sky over (a Fibonacci sphere; those above the horizon count).
#define ATMOSPHERE_AMBIENT_DIRECTIONS 256

// The camera is kept at least this far above the planet's surface, in kilometres, so the horizon stays defined.
#define ATMOSPHERE_MIN_ALTITUDE 0.001

// Invocations per side of the table passes' workgroups.
#define ATMOSPHERE_GROUP_SIDE 8

#endif
