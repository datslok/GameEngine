// Each light probe's offset from its grid point and its state this frame (render/gpu/probe_relocation.h,
// GpuProbeState), in the frame's set 1, as the main pass, the probes' trace and their debug balls read it.
#ifndef SCENE_PROBE_STATES_GLSL
#define SCENE_PROBE_STATES_GLSL

#include "probe_relocation.glsl"

layout(std430, set = 1, binding = 16) readonly buffer ProbeStates {
    ProbeState probeStates[];
};

#endif
