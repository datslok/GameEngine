#ifndef RENDER_GPU_DDGI_SHARED_H
#define RENDER_GPU_DDGI_SHARED_H

// Global illumination's light probes (DDGI; render/gpu/ddgi.h, ddgi.glsl and the passes that include it). Only
// #defines of plain numbers, since GLSL includes this file too; C++ reads them as typed constants in render/gpu/ddgi.h.

// Texels per side of a probe's two octahedral images, without their one-texel border: the light arriving (irradiance)
// and the distances to the nearest surfaces. Both papers' sizes; an 8 x 8 probe is one 64-invocation workgroup.
#define DDGI_IRRADIANCE_TEXELS 8
#define DDGI_DISTANCE_TEXELS 16

// Rays each probe may trace per frame (the papers measured 32 to 256). The update holds a probe's rays in shared memory,
// so the largest count sizes that.
#define DDGI_MIN_RAYS 16
#define DDGI_MAX_RAYS 256

// How sharply a distance texel favours rays near its own direction (a cosine raised to this power), so distances stay
// crisp at the edges of walls [2019].
#define DDGI_DISTANCE_SHARPNESS 50.0

// A ray that met a back face records its distance as this share of the real one: the probe treats whatever lies behind
// as close and hidden, so nothing is lit through it [2021].
#define DDGI_BACK_FACE_DISTANCE_SHARE 0.2

// Irradiance is stored as its fifth root and blended in that space, so bright-to-dark changes do not lag [2021].
#define DDGI_IRRADIANCE_POWER 5.0

// Per-texel speed-ups on the stored (encoded) irradiance [2021]: a change larger than the first threshold lowers the
// texel's hysteresis by the drop; one larger than the second means the scene changed entirely, and the new value is
// taken outright.
#define DDGI_CHANGE_THRESHOLD 0.25
#define DDGI_CHANGE_HYSTERESIS_DROP 0.15
#define DDGI_RESET_THRESHOLD 0.8

// A texel whose rays' summed weight is below this keeps its old value (no ray came near its direction).
#define DDGI_MIN_TEXEL_WEIGHT 1e-6

// The longest distance a distance texel records, in cell diagonals ([2019]: 1.5): far enough to see past a neighbour.
#define DDGI_MAX_DISTANCE_DIAGONALS 1.5

// How far a probe ray looks for a surface, in metres: far enough for any level, so open sky is only where nothing is.
#define DDGI_RAY_REACH 100000.0

// Shadow rays from a probe ray's hit start this far out along the surface's normal, in metres, so they do not hit the
// surface they leave.
#define DDGI_SHADOW_RAY_OFFSET 0.01

// The probe images' largest side in texels, which every GPU with ray tracing supports for 2D images.
#define DDGI_MAX_ATLAS_SIDE 16384

// Invocations per workgroup of the trace pass, one ray each.
#define DDGI_TRACE_GROUP_SIZE 64

#endif
