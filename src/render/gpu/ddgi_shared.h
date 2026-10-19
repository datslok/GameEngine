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

// Sampling the probes at a surface (render/gpu/ddgi_sampling.h, ddgi_sampling.glsl), the 2021 reference code's numbers.
// The self-shadow bias moves the point tested for visibility off the surface: mostly towards the viewer, a little along
// the normal, by this share of the smallest spacing times the tunable bias.
#define DDGI_BIAS_NORMAL_SHARE 0.2
#define DDGI_BIAS_VIEW_SHARE 0.8
#define DDGI_BIAS_SPACING_SHARE 0.75

// A probe behind the surface still counts this much (wrap shading), so small details inside a room keep some probes.
#define DDGI_WRAP_FLOOR 0.2

// Chebyshev's visibility bound is raised to this power to sharpen it, and never drops below the floor, so a point no
// probe can see still gets some light.
#define DDGI_VISIBILITY_POWER 3.0
#define DDGI_MIN_VISIBILITY 0.05

// A probe's weight is at least the smallest weight, and weights below the crush threshold shrink as their cube (scaled to
// meet at the threshold), so faint leaks, which the eye notices most in the dark, fade away.
#define DDGI_SMALLEST_WEIGHT 1e-6
#define DDGI_CRUSH_THRESHOLD 0.2

// Each trilinear factor is at least this, so a probe at a cell's far corner is never weighted to exactly nothing.
#define DDGI_MIN_TRILINEAR 0.001

// A volume lights points out to this many cells beyond its outer probes at full weight, then fades to nothing over the
// next DDGI_VOLUME_FADE cells, where the flat ambient light takes over.
#define DDGI_VOLUME_MARGIN 0.5
#define DDGI_VOLUME_FADE 0.5

// The main pass's specialization constant (constant_id) that switches sampling the probes on. Its pipelines without probes
// have none of the probe code, so pictures without probes are drawn exactly as before probes existed.
#define DDGI_SAMPLE_PROBES_CONSTANT 0

// The most point and spot lights a probe ray's hit may add (each costs a shadow ray); a setting picks how many up to this.
#define DDGI_MAX_HIT_LIGHTS 8

#endif
