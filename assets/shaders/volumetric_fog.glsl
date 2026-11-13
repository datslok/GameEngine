// Volumetric fog (render/gpu/volumetric_fog.h): the height fog's medium, the grid's coordinates, the light the fog
// scatters towards the eye, and the fog along a ray built up slab by slab. Pure functions, mirrored by
// render/gpu/volumetric_fog.cpp, which explains each, and checked against it by the GPU tests; the grid's reads and the
// lights' loops are in the passes (fog_light.comp, fog_integrate.comp, fog_apply.comp) and scene_fog.glsl.

#ifndef VOLUMETRIC_FOG_GLSL
#define VOLUMETRIC_FOG_GLSL

#include "render/gpu/volumetric_fog_shared.h"

// henyeyGreensteinPhase and oneMinusExp, shared with the sky.
#include "atmosphere.glsl"
#include "hash.glsl"

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

vec3 fogFroxelTurn(ivec3 froxel) {
    return vec3(pcg3d(uvec3(froxel)) >> 8u) * (1.0 / 16777216.0);
}

vec3 fogFroxelJitter(vec3 frameJitter, ivec3 froxel) {
    return fract(frameJitter + 0.5 + fogFroxelTurn(froxel)) - 0.5;
}

vec3 fogFroxelCoordinate(ivec3 froxel, ivec3 size, vec3 offset) {
    return clamp((vec3(froxel) + 0.5 + offset) / vec3(size), 0.0, 1.0);
}

vec3 fogHistoryCoordinate(ivec3 froxel, ivec3 size, mat4 inverseViewProjection, mat4 previousViewProjection, vec3 viewForward, float range) {
    vec3 middle = fogVolumePoint(fogFroxelCoordinate(froxel, size, vec3(0.0)), inverseViewProjection, viewForward, range);
    return fogVolumeCoordinate(middle, previousViewProjection, range);
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

vec3 fogRemoved(vec3 seen, FogIntegration fog) {
    float letThrough = max(fog.transmittance, float(FOG_REMOVAL_MIN_TRANSMITTANCE));
    return max((seen - fog.light) * (1.0 / letThrough), vec3(0.0));
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

struct FogMedium {
    vec3 light;
    float extinction;
};

FogMedium fogSliceMedium(FogIntegration before, FogIntegration after, float length) {
    if (!(before.transmittance > 0.0) || !(length > 0.0)) {
        return FogMedium(vec3(0.0), 0.0);
    }

    float through = clamp(after.transmittance / before.transmittance, 0.0, 1.0);
    vec3 light = max((after.light - before.light) * (1.0 / before.transmittance), vec3(0.0));
    float extinction = -log(max(through, exp(-float(FOG_MAX_EXPONENT)))) / length;
    float opaque = 1.0 - through;
    return FogMedium(opaque > 1e-4 ? light * (extinction / opaque) : light * (1.0 / length), extinction);
}

FogMedium mixFogMedium(FogMedium a, FogMedium b, float t) {
    return FogMedium(a.light + (b.light - a.light) * t, a.extinction + (b.extinction - a.extinction) * t);
}

FogIntegration fogThroughMedium(FogMedium medium, float length) {
    float depth = medium.extinction * length;
    float through = exp(-depth);
    return FogIntegration(depth > 1e-4 ? medium.light * ((1.0 - through) / medium.extinction) : medium.light * length, through);
}

struct FogSurfaceRead {
    bool extended;
    int last;
    float mixShare;
    float front;
    float stretch;
};

FogSurfaceRead fogSurfaceRead(float slice, int slices, float range) {
    float count = float(slices);
    float place = slice * count;

    if (place < 1.0 || place >= count) {
        return FogSurfaceRead(false, 0, 0.0, 0.0, 0.0);
    }

    float index = floor(place);
    float front = (place - 1.0) / count;
    return FogSurfaceRead(true, int(index) - 1, place - index, front, fogSliceDepth(slice, range) - fogSliceDepth(front, range));
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
