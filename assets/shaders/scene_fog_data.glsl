// The frame's fog record (render/gpu/volumetric_fog.h) in set 1, binding 27, and the sun's light through the fog above a
// point. Apart from scene_fog.glsl (which includes it) so that surfaces and ray hits, which need only the sun's dimming,
// do not take the fog's grid and its readers with them.
#ifndef SCENE_FOG_DATA_GLSL
#define SCENE_FOG_DATA_GLSL

#include "volumetric_fog.glsl"

// Must match GpuFog in render/gpu/volumetric_fog.h.
layout(std430, set = 1, binding = 27) readonly buffer FogData {
    mat4 viewProjection;         // the grid's camera: camera-relative, without the jitter
    mat4 inverseViewProjection;
    mat4 previousViewProjection; // this frame's camera-relative points to last frame's grid
    vec4 grid;                   // froxels across, down, slices, range (m)
    vec4 options;                // x 1 when on, y the history weight, z 1 reading the probes, w 1 dimming the sun through the fog
    vec4 jitter;                 // xyz this frame's sample offset in froxels
    FogMedia media;              // the layers (extent.z the forward share)
} frameFog;

bool fogOn() {
    return frameFog.options.x > 0.5;
}

// The share of a directional light, per colour, reaching a point at a height (in the world) through the fog above it:
// the fog's own light and surfaces alike (all of it with the setting off, or without fog).
vec3 fogSunThrough(float height, vec3 toLight) {
    return frameFog.options.w > 0.5 ? fogSunTransmittance(frameFog.media, height, toLight.y, frameFog.media.extent.z) : vec3(1.0);
}

#endif
