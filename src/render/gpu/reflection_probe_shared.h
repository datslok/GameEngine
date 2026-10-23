#ifndef RENDER_GPU_REFLECTION_PROBE_SHARED_H
#define RENDER_GPU_REFLECTION_PROBE_SHARED_H

// The reflection probe's numbers, shared by render/gpu/reflection_probe.h and the shaders (reflection_probe.glsl,
// reflections.comp, probe_distance.comp, probe_prefilter.comp and the capture's shaders). Only #defines of plain
// numbers, since GLSL reads them too.

// The capture's nearest distance (metres): nearer things are cut off, and the probe sits at the camera, so nothing
// the camera sees comes nearer than its own near plane.
#define PROBE_NEAR 0.05

// Steps of the parallax search after its first guess (the naive direction): each refines where the reflected ray
// meets what the probe saw.
#define PROBE_SEARCH_STEPS 6

// How far the reflected ray's distance from the probe and the distance the probe saw may disagree, as a share of the
// latter, before the search is trusted less (fully trusted at the first, not at all at the second).
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
