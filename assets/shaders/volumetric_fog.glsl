// Volumetric fog (render/gpu/volumetric_fog.h): the height fog's medium, the grid's coordinates, the light the fog
// scatters towards the eye, and the fog along a ray built up slab by slab. Pure functions, mirrored by
// render/gpu/volumetric_fog.cpp, which explains each, and checked against it by the GPU tests; the grid's reads and the
// lights' loops are in the passes (fog_light.comp, fog_integrate.comp, fog_apply.comp) and scene_fog.glsl.

#ifndef VOLUMETRIC_FOG_GLSL
#define VOLUMETRIC_FOG_GLSL

#include "render/gpu/volumetric_fog_shared.h"

// henyeyGreensteinPhase and oneMinusExp, shared with the sky.
#include "atmosphere.glsl"

// The medium as the frame's fog record holds it: x the density at the base height, y the base height, z the scale
// height, w the anisotropy.
float fogDensity(vec4 medium, float height) {
    return medium.x * exp(min((medium.y - height) / medium.z, float(FOG_MAX_EXPONENT)));
}

// (1 - exp(-x)) / x, with its series near 0.
float fogExponentialShare(float x) {
    if (abs(x) < 1e-3) {
        return 1.0 - x * (0.5 - x / 6.0);
    }

    return (1.0 - exp(-x)) / x;
}

float fogOpticalDepth(vec4 medium, float originHeight, float directionY, float start, float end) {
    if (!(end > start)) {
        return 0.0;
    }

    float rayLength = end - start;
    float change = max(directionY / medium.z * rayLength, -float(FOG_MAX_EXPONENT));
    return fogDensity(medium, originHeight + directionY * start) * rayLength * fogExponentialShare(change);
}

float fogSliceDepth(float slice, float range) {
    return float(FOG_SLICE_START) * (pow(1.0 + range / float(FOG_SLICE_START), slice) - 1.0);
}

float fogDepthSlice(float depth, float range) {
    return log(1.0 + max(depth, 0.0) / float(FOG_SLICE_START)) / log(1.0 + range / float(FOG_SLICE_START));
}

vec3 fogVolumeCoordinate(vec3 relativePoint, mat4 viewProjection, float range) {
    vec4 clip = viewProjection * vec4(relativePoint, 1.0);
    return vec3(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5, fogDepthSlice(clip.w, range));
}

vec3 fogVolumePoint(vec3 coordinate, mat4 inverseViewProjection, vec3 viewForward, float range) {
    vec4 onRay = inverseViewProjection * vec4(coordinate.x * 2.0 - 1.0, 1.0 - coordinate.y * 2.0, 0.5, 1.0);
    vec3 direction = normalize(onRay.xyz);
    return direction * (fogSliceDepth(coordinate.z, range) / dot(direction, viewForward));
}

vec3 fogInScattering(vec3 irradiance, vec3 toLight, vec3 viewDirection, float anisotropy) {
    return irradiance * (ATMOSPHERE_PI * henyeyGreensteinPhase(anisotropy, dot(viewDirection, toLight)));
}

vec3 fogAmbient(vec3 irradianceUp, vec3 irradianceDown) {
    return (irradianceUp + irradianceDown) * 0.5;
}

// The fog along a ray so far: the light it gathered and the share of what lies behind it lets through.
struct FogIntegration {
    vec3 light;
    float transmittance;
};

void fogIntegrateSlice(inout FogIntegration sum, vec3 scattered, float extinction, float sliceLength) {
    float depth = extinction * sliceLength;
    sum.light += scattered * (sum.transmittance * oneMinusExp(depth));
    sum.transmittance *= exp(-depth);
}

FogIntegration fogFollow(FogIntegration front, FogIntegration behind) {
    return FogIntegration(front.light + behind.light * front.transmittance, front.transmittance * behind.transmittance);
}

vec3 fogComposite(vec3 colour, FogIntegration fog) {
    return colour * fog.transmittance + fog.light;
}

vec3 fogOverGlass(vec3 added, vec3 transmittance, FogIntegration fog) {
    return added * fog.transmittance + (vec3(1.0) - transmittance) * fog.light;
}

// x: where the integrated grid is read in depth, y: the share of that reading (towards no fog at the camera).
vec2 fogSliceRead(float slice, int slices) {
    float count = float(slices);
    float halfTexel = 0.5 / count;
    return vec2(clamp(slice - halfTexel, halfTexel, 1.0 - halfTexel), clamp(slice * count, 0.0, 1.0));
}

float fogHistoryWeight(vec3 previousCoordinate, float weight) {
    bool inside = all(greaterThanEqual(previousCoordinate, vec3(0.0))) && all(lessThanEqual(previousCoordinate, vec3(1.0)));
    return inside ? weight : 0.0;
}

FogIntegration fogBeyondGrid(vec4 medium, vec3 source, float originHeight, float directionY, float start, float end) {
    float depth = fogOpticalDepth(medium, originHeight, directionY, start, end);
    return FogIntegration(source * oneMinusExp(depth), exp(-depth));
}

#endif
