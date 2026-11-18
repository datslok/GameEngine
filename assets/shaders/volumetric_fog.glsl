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

// A layer as the frame's fog record holds it: medium x the density at the base height, y the base height (a pool's
// top), z the scale height (a pool's soft edge), w the anisotropy; shape 0 exponential or FOG_SHAPE_POOLED.
float fogDensity(vec4 medium, float shape, float height) {
    if (shape == float(FOG_SHAPE_POOLED)) {
        return medium.x * exp(-max(height - medium.y, 0.0) / medium.z);
    }

    return medium.x * exp(min((medium.y - height) / medium.z, float(FOG_MAX_EXPONENT)));
}

// (1 - exp(-x)) / x, with its series near 0.
float fogExponentialShare(float x) {
    if (abs(x) < 1e-3) {
        return 1.0 - x * (0.5 - x / 6.0);
    }

    return (1.0 - exp(-x)) / x;
}

// A pooled layer's depth from start to end (end past start): constant below the top, the exponential above it from
// that part's lower, densest end.
float fogPooledOpticalDepth(vec4 medium, float originHeight, float directionY, float start, float end) {
    if (directionY == 0.0) {
        return fogDensity(medium, float(FOG_SHAPE_POOLED), originHeight) * (end - start);
    }

    float crossing = (medium.y - originHeight) / directionY;
    bool rising = directionY > 0.0;
    float belowStart = rising ? start : max(start, crossing);
    float belowEnd = rising ? min(end, crossing) : end;
    float aboveStart = rising ? max(start, crossing) : start;
    float aboveEnd = rising ? end : min(end, crossing);
    float depth = 0.0;

    if (belowEnd > belowStart) {
        depth += medium.x * (belowEnd - belowStart);
    }

    if (aboveEnd > aboveStart) {
        float rayLength = aboveEnd - aboveStart;
        float lowest = originHeight + directionY * (rising ? aboveStart : aboveEnd);
        depth += fogDensity(medium, float(FOG_SHAPE_POOLED), lowest) * rayLength * fogExponentialShare(abs(directionY) / medium.z * rayLength);
    }

    return depth;
}

float fogOpticalDepth(vec4 medium, float shape, float originHeight, float directionY, float start, float end) {
    if (!(end > start)) {
        return 0.0;
    }

    if (shape == float(FOG_SHAPE_POOLED)) {
        return fogPooledOpticalDepth(medium, originHeight, directionY, start, end);
    }

    float rayLength = end - start;
    float change = max(directionY / medium.z * rayLength, -float(FOG_MAX_EXPONENT));
    return fogDensity(medium, 0.0, originHeight + directionY * start) * rayLength * fogExponentialShare(change);
}

// The layers (render/gpu/volumetric_fog.h's GpuFogMedia): per layer its medium and (albedo, farthest distance), the
// shapes, and the extent (x the layer count, y the farthest distance).
struct FogMedia {
    vec4 layer[FOG_MAX_LAYERS];
    vec4 albedo[FOG_MAX_LAYERS];
    vec4 shape;
    vec4 extent;
};

int fogLayerCount(FogMedia media) {
    return int(media.extent.x);
}

float fogMediaDensity(FogMedia media, float height, float distance) {
    float density = 0.0;

    for (int i = 0; i < fogLayerCount(media); ++i) {
        density += distance <= media.albedo[i].w ? fogDensity(media.layer[i], media.shape[i], height) : 0.0;
    }

    return density;
}

float fogMediaOpticalDepth(FogMedia media, float originHeight, float directionY, float start, float end) {
    float depth = 0.0;

    for (int i = 0; i < fogLayerCount(media); ++i) {
        depth += fogOpticalDepth(media.layer[i], media.shape[i], originHeight, directionY, start, min(end, media.albedo[i].w));
    }

    return depth;
}

vec4 fogMediaShares(FogMedia media, float height, float distance) {
    vec4 shares = vec4(0.0);

    for (int i = 0; i < fogLayerCount(media); ++i) {
        shares[i] = distance <= media.albedo[i].w ? fogDensity(media.layer[i], media.shape[i], height) : 0.0;
    }

    float total = shares.x + shares.y + shares.z + shares.w;
    return total > 0.0 ? shares / total : vec4(0.0);
}

vec4 fogMediaStretchShares(FogMedia media, float originHeight, float directionY, float start, float end) {
    vec4 shares = vec4(0.0);

    for (int i = 0; i < fogLayerCount(media); ++i) {
        shares[i] = fogOpticalDepth(media.layer[i], media.shape[i], originHeight, directionY, start, min(end, media.albedo[i].w));
    }

    float total = shares.x + shares.y + shares.z + shares.w;
    return total > 0.0 ? shares / total : vec4(0.0);
}

vec3 fogSunTransmittance(FogMedia media, float height, float toLightY, float forwardShare) {
    vec3 depth = vec3(0.0);

    for (int i = 0; i < fogLayerCount(media); ++i) {
        vec3 lost = vec3(1.0) - media.albedo[i].rgb * (forwardShare * media.layer[i].w);
        depth += lost * fogOpticalDepth(media.layer[i], media.shape[i], height, toLightY, 0.0, media.albedo[i].w);
    }

    return exp(-depth);
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

vec3 fogMediaInScattering(FogMedia media, vec4 shares, vec3 irradiance, vec3 toLight, vec3 viewDirection) {
    vec3 light = vec3(0.0);

    for (int i = 0; i < fogLayerCount(media); ++i) {
        light += fogInScattering(irradiance, toLight, viewDirection, media.layer[i].w) * media.albedo[i].rgb * shares[i];
    }

    return light;
}

vec3 fogMediaAmbient(FogMedia media, vec4 shares, vec3 ambient) {
    vec3 albedo = vec3(0.0);

    for (int i = 0; i < fogLayerCount(media); ++i) {
        albedo += media.albedo[i].rgb * shares[i];
    }

    return ambient * albedo;
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

FogIntegration fogBeyondGrid(FogMedia media, vec3 source, float originHeight, float directionY, float start, float end) {
    float depth = fogMediaOpticalDepth(media, originHeight, directionY, start, end);
    return FogIntegration(source * oneMinusExp(depth), exp(-depth));
}

// One exponential layer's.
float fogFarSampleDistance(vec4 medium, float originHeight, float directionY, float start, float end, float share) {
    if (!(end > start)) {
        return start;
    }

    float rayLength = end - start;
    float total = fogOpticalDepth(medium, 0.0, originHeight, directionY, start, end);
    float density = fogDensity(medium, 0.0, originHeight + directionY * start);

    if (!(total > 0.0) || !(density > 0.0)) {
        return start + share * rayLength;
    }

    float depth = -log(1.0 - share * oneMinusExp(total));

    if (!(depth < total)) {
        return end;
    }

    float x = directionY / medium.z * depth / density;
    float stretch = abs(x) < 1e-3 ? 1.0 + x * (0.5 + x / 3.0) : -log(max(1.0 - x, 1e-30)) / x;
    return clamp(start + depth / density * stretch, start, end);
}

float fogMediaFarSampleDistance(FogMedia media, float originHeight, float directionY, float start, float end, float share) {
    if (fogLayerCount(media) == 1 && media.shape.x != float(FOG_SHAPE_POOLED)) {
        return fogFarSampleDistance(media.layer[0], originHeight, directionY, start, min(end, media.albedo[0].w), share);
    }

    if (!(end > start)) {
        return start;
    }

    float total = fogMediaOpticalDepth(media, originHeight, directionY, start, end);

    if (!(total > 0.0)) {
        return start + share * (end - start);
    }

    float depth = -log(1.0 - share * oneMinusExp(total));

    if (!(depth < total)) {
        return end;
    }

    float first = fogMediaDensity(media, originHeight + directionY * start, start);
    float low = start;
    float high = end;
    float distance = first > 0.0 ? min(start + depth / first, end) : 0.5 * (start + end);

    for (int step = 0; step < FOG_FAR_SAMPLE_NEWTON_STEPS; ++step) {
        float error = fogMediaOpticalDepth(media, originHeight, directionY, start, distance) - depth;

        if (error == 0.0) {
            break;
        }

        if (error > 0.0) {
            high = distance;
        } else {
            low = distance;
        }

        float density = fogMediaDensity(media, originHeight + directionY * distance, distance);
        float next = density > 0.0 ? distance - error / density : low - 1.0;
        distance = next >= low && next <= high ? next : 0.5 * (low + high);
    }

    return clamp(distance, start, end);
}

#endif
