// The probes every light probe pass reads, as push constants (render/gpu/ddgi.h, GpuProbeVolume), and what follows from
// them. Declarations and small helpers only. Where each cascade's probes are is in the cascades' buffer
// (probe_cascades.glsl), which each pass declares where it binds it.
#ifndef DDGI_VOLUME_GLSL
#define DDGI_VOLUME_GLSL

// Must match GpuProbeVolume in render/gpu/ddgi.h.
layout(push_constant) uniform ProbeVolumeData {
    ivec4 counts;     // xy the atlases' columns and rows, w the ray stride (room for the most rays any probe traces)
    ivec4 cascades;   // x how many cascades, y every cascade's probes together
    vec4 rotation[3]; // this frame's ray rotation, its matrix's rows in xyz; w: still probes' rays, fast probes' rays, the change window's share kept
    vec4 sky;         // rgb the light rays that hit nothing bring back, w the change threshold (0: fixed memory)
    vec4 hysteresis;  // x the irradiance's, y the distances' share kept this frame, z the debug balls' radius, w the ray phase
    vec4 keeps;       // x the fast irradiance's share kept this frame
} volume;

// Every cascade's probes together.
int volumeProbeCount() {
    return volume.cascades.y;
}

// The atlases' columns and rows of squares, every cascade's probes one after another (ddgi.h, probeAtlasGrid).
ivec2 volumeAtlasGrid() {
    return volume.counts.xy;
}

// A whole number the push constants carry as a float.
int volumeWholeNumber(float value) {
    return int(value + 0.5);
}

// The rays a probe traces this frame: more within reach of a fast light.
int volumeRayCount(bool nearFastLight) {
    return volumeWholeNumber(nearFastLight ? volume.rotation[1].w : volume.rotation[0].w);
}

// A probe's own turn of its rays (render/gpu/ddgi.h, GpuProbeTurn), as the trace and the update read it from their
// ProbeTurns buffer: identity rows unless probes share light.
struct ProbeTurn {
    vec4 rows[3];
};

vec3 volumeRayDirection(int ray, int count, ProbeTurn probeTurn) {
    vec3 rotation[3] = vec3[3](volume.rotation[0].xyz, volume.rotation[1].xyz, volume.rotation[2].xyz);
    vec3 turn[3] = vec3[3](probeTurn.rows[0].xyz, probeTurn.rows[1].xyz, probeTurn.rows[2].xyz);
    return probeRayDirection(ray, count, volumeWholeNumber(volume.hysteresis.w), rotation, turn);
}

// A fast light's part of a ray's light, packed as half floats (8 bytes a ray instead of 16), and back.
uvec2 packFastRayLight(vec3 light) {
    return uvec2(packHalf2x16(light.rg), packHalf2x16(vec2(light.b, 0.0)));
}

vec3 unpackFastRayLight(uvec2 packed) {
    return vec3(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y).x);
}

#endif
