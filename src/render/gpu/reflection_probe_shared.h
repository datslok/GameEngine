#ifndef RENDER_GPU_REFLECTION_PROBE_SHARED_H
#define RENDER_GPU_REFLECTION_PROBE_SHARED_H

// The reflection probe's numbers, shared by render/gpu/reflection_probe.h and the shaders (reflection_probe.glsl,
// reflections.comp, probe_distance.comp, probe_prefilter.comp and the capture's shaders). Only #defines of plain
// numbers, since GLSL reads them too.

// The capture's nearest distance (metres): nearer things are cut off, and the probe sits at the camera, so nothing
// the camera sees comes nearer than its own near plane.
#define PROBE_NEAR 0.05

// The parallax search marches along the reflected ray from this far (metres) to beyond the capture's far plane, in
// this many steps, each the same factor longer than the last (32 steps to 300 m: about a third longer each), then
// halves the step where the ray first went behind what the probe saw this many times (256 times narrower).
#define PROBE_MARCH_START 0.02
#define PROBE_MARCH_STEPS 32
#define PROBE_REFINE_STEPS 8

// How far the distances the probe saw on either side of that crossing may disagree, as a share of the farther, before
// the search is trusted less (fully at the first, not at all at the second): a real surface is continuous there, an
// edge the probe saw past is a jump.
#define PROBE_MATCH_SHARE 0.03
#define PROBE_MISMATCH_SHARE 0.15

// Directions each prefiltered texel averages, drawn from its roughness's GGX lobe.
#define PROBE_PREFILTER_SAMPLES 64

// The prefiltered cube's smallest level is this many texels a side: smaller ones would only blur roughness 1 more.
#define PROBE_SMALLEST_LEVEL_SIZE 4

// The capture's sun shadow ray past the cascades (on GPUs with ray queries): lifted off its surface by this much plus
// this much per metre from the camera (the capture's texels grow with distance), reaching this far.
#define PROBE_SHADOW_RAY_OFFSET 0.01
#define PROBE_SHADOW_RAY_OFFSET_PER_METRE 0.002
#define PROBE_SHADOW_RAY_REACH 100000.0

// The probe passes' workgroups: a square of this many texels a side, one texel an invocation.
#define PROBE_GROUP_SIDE 8

// The specialization constant of triangle.vert that has it take each cube face's view-projection from push constants.
#define PROBE_CAPTURE_VERTEX_CONSTANT 0

#endif
