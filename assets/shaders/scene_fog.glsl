// The frame's fog (render/gpu/volumetric_fog.h) in set 1: its record (binding 27) and the fog integrated through its grid
// (binding 28), and the fog along the view to a point: the grid's up to its range, then the height fog's closed form lit
// by the directional lights and the flat ambient (without shadows) out to the fog's farthest distance. Included by the
// fog's passes (fog_light.comp, fog_integrate.comp, fog_apply.comp) and glass (scene_surface.glsl), after
// frame_data.glsl, scene_lights.glsl and scene_sky.glsl.
#ifndef SCENE_FOG_GLSL
#define SCENE_FOG_GLSL

#include "render/gpu/sky_visibility_shared.h"
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

// The integrated grid's texel k (the fog from the camera to the far side of slice k) at a place across the screen;
// the camera's, no fog, for k = -1.
FogIntegration fogVolumeTexel(vec2 across, int k) {
    if (k < 0) {
        return FogIntegration(vec3(0.0), 1.0);
    }

    vec4 texel = textureLod(fogVolume, vec3(across, (float(k) + 0.5) / frameFog.grid.z), 0.0);
    return FogIntegration(texel.rgb, texel.a);
}

// Slice k's medium (fogSliceMedium) at a place across the screen, from the texels either side of it (the first
// slice's for k below 0).
FogMedium fogVolumeMedium(vec2 across, int k) {
    int slice = max(k, 0);
    int slices = int(frameFog.grid.z);
    float range = frameFog.grid.w;
    float length = fogSliceDepth(float(slice + 1) / float(slices), range) - fogSliceDepth(float(slice) / float(slices), range);
    return fogSliceMedium(fogVolumeTexel(across, slice - 1), fogVolumeTexel(across, slice), length);
}

/*
* The fog to a surface at a place in the grid (fogSurfaceRead): never the slice the surface lies in, whose jittered light
* sample may lie behind it, but the grid up to one slice before the surface and over the last slice's depth a medium
* sliding between the two slices before, continuous across slice boundaries; in the first slice, and past the grid's
* end, the grid as fogVolumeAt reads it.
*/
FogIntegration fogVolumeToSurface(vec3 coordinate) {
    FogSurfaceRead read = fogSurfaceRead(coordinate.z, int(frameFog.grid.z), frameFog.grid.w);

    if (!read.extended) {
        return fogVolumeAt(coordinate);
    }

    FogMedium medium = mixFogMedium(fogVolumeMedium(coordinate.xy, read.last - 1), fogVolumeMedium(coordinate.xy, read.last), read.mixShare);
    return fogFollow(fogVolumeAt(vec3(coordinate.xy, read.front)), fogThroughMedium(medium, read.stretch));
}

// The flat ambient the fog past the grid scatters (before the albedo): what a surface facing up gets; the air in the
// open gets the open field's share of it (SKY_OPEN_FIELD_OPEN: the sky above, not the ground below), as the sky
// visibility grid and the probes give the air nearer, unless neither is read, when all the fog takes the flat ambient alike.
vec3 fogBeyondAmbient() {
    bool airModelled = skyVisibilityOptions.x > 0.5 || frameFog.options.z > 0.5;
    return flatAmbient() * (airModelled ? float(SKY_OPEN_FIELD_OPEN) : 1.0);
}

// The light the fog past the grid scatters towards the eye along a view direction per unit of optical depth: the
// directional lights' (as they reach the ground, without shadows) and the flat ambient, through the albedo.
vec3 fogBeyondSource(vec3 viewDirection) {
    vec3 light = fogBeyondAmbient();

    for (int i = 0; i < counts.x; ++i) {
        light += fogInScattering(directional[i].radiance.rgb, directional[i].toLight.xyz, viewDirection, frameFog.medium.w);
    }

    return light * frameFog.albedo.rgb;
}

// The grid's fog between the camera and a point (measured from the camera), or along its ray through the whole grid
// for the sky.
FogIntegration fogNearView(vec3 relativePoint, bool sky) {
    vec3 coordinate = fogVolumeCoordinate(relativePoint, frameFog.viewProjection, frameFog.grid.w);
    return sky ? fogVolumeAt(vec3(coordinate.xy, 1.0)) : fogVolumeToSurface(coordinate);
}

// The stretch of a view ray past the grid: its direction, from the grid's end (the range over the ray's cosine to the
// view axis) to the point, or for the sky the fog's farthest distance; empty (end not past start) inside the grid.
struct FogFarStretch {
    vec3 direction;
    float start;
    float end;
};

FogFarStretch fogFarStretch(vec3 relativePoint, bool sky) {
    vec3 direction = normalize(relativePoint);
    float end = min(sky ? frameFog.albedo.w : length(relativePoint), frameFog.albedo.w);
    return FogFarStretch(direction, frameFog.grid.w / dot(direction, viewForward.xyz), end);
}

// The closed form's fog over a stretch past the grid, lit by a source (the light per unit of optical depth).
FogIntegration fogOverStretch(FogFarStretch stretch, vec3 source) {
    return fogBeyondGrid(frameFog.medium, source, cameraOrigin.y, stretch.direction.y, stretch.start, stretch.end);
}

/*
* The fog between the camera and a point (measured from the camera), or along the ray through it out to the fog's
* farthest distance for the sky (beyond everything drawn): the grid up to its range, then the closed form, sunlit
* throughout (the picture pass shades it: scene_fog_shadows.glsl, fogAlongViewShadowed).
*/
FogIntegration fogAlongView(vec3 relativePoint, bool sky) {
    FogIntegration fog = fogNearView(relativePoint, sky);
    FogFarStretch stretch = fogFarStretch(relativePoint, sky);

    if (stretch.end > stretch.start) {
        fog = fogFollow(fog, fogOverStretch(stretch, fogBeyondSource(stretch.direction)));
    }

    return fog;
}

#endif
