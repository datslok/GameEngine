// The probe volume every light probe pass reads, as push constants (render/gpu/ddgi.h, GpuProbeVolume), and what
// follows from it. Declarations and small helpers only.
#ifndef DDGI_VOLUME_GLSL
#define DDGI_VOLUME_GLSL

// Must match GpuProbeVolume in render/gpu/ddgi.h.
layout(push_constant) uniform ProbeVolumeData {
    vec4 origin;      // xyz the first probe, w the fast irradiance's share kept this frame
    vec4 spacing;     // xyz the spacing between probes, w the longest distance recorded
    ivec4 counts;     // xyz probes along each axis, w the ray stride (room for the most rays any probe traces)
    vec4 rotation[3]; // this frame's ray rotation, its matrix's rows in xyz; w: still probes' rays, fast probes' rays, unused
    vec4 sky;         // rgb the light rays that hit nothing bring back
    vec4 hysteresis;  // x the irradiance's, y the distances' share kept this frame, z the debug balls' radius, w the ray phase
} volume;

int volumeProbeCount() {
    return volume.counts.x * volume.counts.y * volume.counts.z;
}

vec3 volumeProbePosition(int probe) {
    return probePosition(volume.origin.xyz, volume.spacing.xyz, volume.counts.xyz, probe);
}

// A whole number the push constants carry as a float.
int volumeWholeNumber(float value) {
    return int(value + 0.5);
}

// The rays a probe traces this frame: more within reach of a fast light.
int volumeRayCount(bool nearFastLight) {
    return volumeWholeNumber(nearFastLight ? volume.rotation[1].w : volume.rotation[0].w);
}

vec3 volumeRayDirection(int ray, int count) {
    vec3 rotation[3] = vec3[3](volume.rotation[0].xyz, volume.rotation[1].xyz, volume.rotation[2].xyz);
    return probeRayDirection(ray, count, volumeWholeNumber(volume.hysteresis.w), rotation);
}

// A fast light's part of a ray's light, packed as half floats (8 bytes a ray instead of 16), and back.
uvec2 packFastRayLight(vec3 light) {
    return uvec2(packHalf2x16(light.rg), packHalf2x16(vec2(light.b, 0.0)));
}

vec3 unpackFastRayLight(uvec2 packed) {
    return vec3(unpackHalf2x16(packed.x), unpackHalf2x16(packed.y).x);
}

#endif
