#ifndef RENDER_GPU_REFLECTION_PROBE_SHARED_H
#define RENDER_GPU_REFLECTION_PROBE_SHARED_H

// The reflection probe's numbers, shared by render/gpu/reflection_probe.h and the shaders (reflection_probe.glsl,
// reflections.comp, probe_distance.comp, probe_prefilter.comp and the capture's shaders). Only #defines of plain
// numbers, since GLSL reads them too.

// The capture's nearest distance (metres): nearer things are cut off, and the probe sits at the camera, so nothing
// the camera sees comes nearer than its own near plane.
#define PROBE_NEAR 0.05

// The parallax search leaves the surface from this far along the reflected ray (metres), doubling at most this many
// times until the ray is in front of what the probe saw; then walks the distances' pyramid cell by cell, at most this
// many steps (across a cell whose nearest distance the ray stays in front of, a level up; else a level down); then it
// halves the last texel's stretch of the ray this many times (256 times narrower) to find where it went behind.
#define PROBE_MARCH_START 0.02
#define PROBE_LEAVE_STEPS 8
#define PROBE_MARCH_ITERATIONS 64
#define PROBE_REFINE_STEPS 8

// A step out of a cell lands this share of its t (at least the second, in metres) past the cell's edge, so it is
// inside the next cell and never stuck on the edge.
#define PROBE_CELL_CROSSING_SHARE 0.0001
#define PROBE_CELL_CROSSING 0.00001

// The ray starts this many of the probe's texels (at the point's distance) off its surface, more where the probe sees
// the surface at a slant: over the cosine between its normal and the way to the probe, at least this.
#define PROBE_START_LIFT_TEXELS 2.0
#define PROBE_START_LIFT_SLANT 0.25

// How far the four texels the blended distance at the crossing is made of may spread, as a share of the farthest, before
// the crossing is taken for a blend between two surfaces (an edge) rather than one surface seen at a slant (a floor at a
// few degrees changes far less across them): trusted fully up to the first, not at all from the second.
#define PROBE_EDGE_JUMP_START 0.2
#define PROBE_EDGE_JUMP_END 0.4

// How far the (blended) distances the probe saw on either side of that crossing may disagree, as a share of the farther,
// before the search is trusted less (fully at the first, not at all at the second): a real surface is continuous there,
// an edge the probe saw past is a jump.
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
