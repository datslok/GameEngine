// The frame's fog (render/gpu/volumetric_fog.h) in set 1: its record (binding 27) and the fog integrated through its grid
// (binding 28), and the fog along the view to a point: the grid's up to its range, then the height fog's closed form lit
// by the directional lights and the flat ambient (without shadows) out to the fog's farthest distance. Included by the
// fog's passes (fog_light.comp, fog_integrate.comp, fog_apply.comp) and glass (scene_surface.glsl), after
// frame_data.glsl, scene_lights.glsl and scene_sky.glsl.
#ifndef SCENE_FOG_GLSL
#define SCENE_FOG_GLSL

#include "volumetric_fog.glsl"

// Must match GpuFog in render/gpu/volumetric_fog.h.
layout(std430, set = 1, binding = 27) readonly buffer FogData {
    mat4 viewProjection;         // the grid's camera: camera-relative, without the jitter
    mat4 inverseViewProjection;
    mat4 previousViewProjection; // this frame's camera-relative points to last frame's grid
    vec4 grid;                   // froxels across, down, slices, range (m)
    vec4 medium;                 // density at the base height, base height, scale height, anisotropy
    vec4 albedo;                 // rgb albedo, w the farthest distance along a ray (m)
    vec4 options;                // x 1 when on, y the history weight, z 1 reading the probes
    vec4 jitter;                 // xyz this frame's sample offset in froxels
} frameFog;

// The light gathered (rgb) and let through (alpha) from the camera to the far side of each slice (a clear stand-in
// without fog).
layout(set = 1, binding = 28) uniform sampler3D fogVolume;

bool fogOn() {
    return frameFog.options.x > 0.5;
}

// The integrated grid at a place in it (a slice coordinate past 1 reads the whole grid).
FogIntegration fogVolumeAt(vec3 coordinate) {
    vec2 read = fogSliceRead(coordinate.z, int(frameFog.grid.z));
    vec4 texel = textureLod(fogVolume, vec3(coordinate.xy, read.x), 0.0);
    return FogIntegration(texel.rgb * read.y, mix(1.0, texel.a, read.y));
}

// The light the fog past the grid scatters towards the eye along a view direction per unit of optical depth: the
// directional lights' (as they reach the ground) and the flat ambient, through the albedo.
vec3 fogBeyondSource(vec3 viewDirection) {
    vec3 light = flatAmbient();

    for (int i = 0; i < counts.x; ++i) {
        light += fogInScattering(directional[i].radiance.rgb, directional[i].toLight.xyz, viewDirection, frameFog.medium.w);
    }

    return light * frameFog.albedo.rgb;
}

/*
* The fog between the camera and a point (measured from the camera), or along the ray through it out to the fog's
* farthest distance for the sky (beyond everything drawn): the grid up to its range, then the closed form.
*/
FogIntegration fogAlongView(vec3 relativePoint, bool sky) {
    float range = frameFog.grid.w;
    vec3 direction = normalize(relativePoint);
    vec3 coordinate = fogVolumeCoordinate(relativePoint, frameFog.viewProjection, range);
    FogIntegration fog = fogVolumeAt(vec3(coordinate.xy, sky ? 1.0 : coordinate.z));

    float gridEnd = range / dot(direction, viewForward.xyz);
    float end = min(sky ? frameFog.albedo.w : length(relativePoint), frameFog.albedo.w);

    if (end > gridEnd) {
        fog = fogFollow(fog, fogBeyondGrid(frameFog.medium, fogBeyondSource(direction), cameraOrigin.y, direction.y, gridEnd, end));
    }

    return fog;
}

#endif
