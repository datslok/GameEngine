// The light probes' cascades (render/gpu/probe_cascades.h): nested windows of probes round the camera, each a scrolled
// grid (ddgi.glsl, probeSlot) whose probes are numbered from its first among all the cascades'. The record and small
// helpers only; each pass declares the buffer it reads them from.
#ifndef PROBE_CASCADES_GLSL
#define PROBE_CASCADES_GLSL

#include "ddgi.glsl"
#include "ddgi_sampling.glsl"

// Must match GpuProbeCascade in render/gpu/probe_cascades.h (std430). Positions are measured from the camera.
struct ProbeCascade {
    vec4 origin;       // xyz the window's first probe, w the longest distance its probes record
    vec4 spacing;      // xyz the spacing between probes
    ivec4 scroll;      // xyz the storage's scroll, w the cascade's first probe
    vec4 weightCentre; // xyz where its weight is centred
    vec4 weightReach;  // xyz how far from there it lights
    vec4 weightFade;   // xyz how far its weight takes to fall to nothing
    ivec4 counts;      // xyz its probes along each axis
};

// Where the grid puts probe number probe of this cascade.
vec3 cascadeProbePosition(ProbeCascade cascade, int probe) {
    return probePosition(cascade.origin.xyz, cascade.spacing.xyz, cascade.counts.xyz, cascade.scroll.xyz, probe - cascade.scroll.w);
}

// The probe number of the probe at grid place grid (within the cascade) of this cascade.
int cascadeProbeNumber(ProbeCascade cascade, ivec3 grid) {
    return cascade.scroll.w + probeSlot(cascade.counts.xyz, cascade.scroll.xyz, grid);
}

float cascadeWeight(ProbeCascade cascade, vec3 point) {
    return probeCascadeWeight(cascade.weightCentre.xyz, cascade.weightReach.xyz, cascade.weightFade.xyz, point);
}

#endif
