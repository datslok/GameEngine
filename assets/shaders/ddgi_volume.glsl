// The probe volume every light probe pass reads, as push constants (render/gpu/ddgi.h, GpuProbeVolume), and what
// follows from it. Declarations and small helpers only.
#ifndef DDGI_VOLUME_GLSL
#define DDGI_VOLUME_GLSL

// Must match GpuProbeVolume in render/gpu/ddgi.h.
layout(push_constant) uniform ProbeVolumeData {
    vec4 origin;      // xyz the first probe
    vec4 spacing;     // xyz the spacing between probes, w the longest distance recorded
    ivec4 counts;     // xyz probes along each axis, w rays per probe this frame
    vec4 rotation[3]; // this frame's ray rotation, its matrix's rows in xyz
    vec4 sky;         // rgb the light rays that hit nothing bring back
    vec4 hysteresis;  // x the irradiance's, y the distances' share kept this frame, z the debug balls' radius, w the ray phase
} volume;

int volumeProbeCount() {
    return volume.counts.x * volume.counts.y * volume.counts.z;
}

vec3 volumeProbePosition(int probe) {
    return probePosition(volume.origin.xyz, volume.spacing.xyz, volume.counts.xyz, probe);
}

vec3 volumeRayDirection(int ray) {
    vec3 rotation[3] = vec3[3](volume.rotation[0].xyz, volume.rotation[1].xyz, volume.rotation[2].xyz);
    return probeRayDirection(ray, volume.counts.w, int(volume.hysteresis.w + 0.5), rotation);
}

#endif
