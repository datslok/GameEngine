// The light probes' cascades this frame (render/gpu/probe_cascades.h, GpuProbeCascade) in the frame's set 1, as the
// main pass, the probes' trace, seeds and relocation, and their debug balls read them.
#ifndef SCENE_PROBE_CASCADES_GLSL
#define SCENE_PROBE_CASCADES_GLSL

#include "probe_cascades.glsl"

layout(std430, set = 1, binding = 20) readonly buffer ProbeCascades {
    ProbeCascade probeCascades[];
};

#endif
