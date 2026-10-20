#ifndef DDGI_GLSL
#define DDGI_GLSL

// Global illumination's light probes (DDGI): the octahedral images and their borders, the rays' directions, the
// update's weights and blending, and where each probe lives in the atlases. Pure functions, mirrored by
// render/gpu/ddgi.h, which explains each; the GPU tests check them against it.

#include "render/gpu/bloom_shared.h"
#include "render/gpu/ddgi_shared.h"

// 1 for zero and up, -1 below: the octahedral fold needs a sign that is never 0.
vec2 signNotZero(vec2 value) {
    return vec2(value.x >= 0.0 ? 1.0 : -1.0, value.y >= 0.0 ? 1.0 : -1.0);
}

vec2 octahedralEncode(vec3 unitDirection) {
    vec2 square = unitDirection.xy / (abs(unitDirection.x) + abs(unitDirection.y) + abs(unitDirection.z));

    if (unitDirection.z < 0.0) {
        square = (1.0 - abs(square.yx)) * signNotZero(square);
    }

    return square;
}

vec3 octahedralDecode(vec2 square) {
    vec3 direction = vec3(square, 1.0 - abs(square.x) - abs(square.y));

    if (direction.z < 0.0) {
        direction.xy = (1.0 - abs(direction.yx)) * signNotZero(direction.xy);
    }

    return normalize(direction);
}

vec3 probeTexelDirection(ivec2 texel, int side) {
    return octahedralDecode((vec2(texel) + 0.5) / float(side) * 2.0 - 1.0);
}

ivec2 probeBorderSource(ivec2 bordered, int side) {
    int last = side + 1;
    bool onColumn = bordered.x == 0 || bordered.x == last;
    bool onRow = bordered.y == 0 || bordered.y == last;

    if (onColumn && onRow) {
        return ivec2(bordered.x == 0 ? side : 1, bordered.y == 0 ? side : 1);
    }

    if (onColumn) {
        return ivec2(bordered.x == 0 ? 1 : side, last - bordered.y);
    }

    if (onRow) {
        return ivec2(last - bordered.x, bordered.y == 0 ? 1 : side);
    }

    return bordered;
}

int probeBorderTexelCount(int side) {
    return 4 * side + 4;
}

ivec2 probeBorderTexel(int number, int side) {
    int last = side + 1;

    if (number < side) {
        return ivec2(1 + number, 0);
    }

    if (number < 2 * side) {
        return ivec2(1 + number - side, last);
    }

    if (number < 3 * side) {
        return ivec2(0, 1 + number - 2 * side);
    }

    if (number < 4 * side) {
        return ivec2(last, 1 + number - 3 * side);
    }

    int corner = number - 4 * side;
    return ivec2((corner & 1) != 0 ? last : 0, (corner & 2) != 0 ? last : 0);
}

vec3 sphericalFibonacci(int index, int count) {
    const float goldenFraction = 0.6180339887;
    const float fullTurn = 6.28318530718;
    float z = 1.0 - (2.0 * float(index) + 1.0) / float(count);
    float angle = fullTurn * fract(float(index) * goldenFraction);
    float radius = sqrt(max(0.0, 1.0 - z * z));
    return vec3(radius * cos(angle), radius * sin(angle), z);
}

// phase: the frame's place in its ray cycle; rotation: the cycle's rotation matrix's rows.
vec3 probeRayDirection(int ray, int count, int phase, vec3 rotation[3]) {
    vec3 point = sphericalFibonacci(ray * DDGI_RAY_CYCLE + phase, count * DDGI_RAY_CYCLE);
    return vec3(dot(rotation[0], point), dot(rotation[1], point), dot(rotation[2], point));
}

vec3 probeRayDirection(int ray, int count, int phase, vec3 rotation[3], vec3 turn[3]) {
    vec3 point = sphericalFibonacci(ray * DDGI_RAY_CYCLE + phase, count * DDGI_RAY_CYCLE);
    vec3 turned = vec3(dot(turn[0], point), dot(turn[1], point), dot(turn[2], point));
    return vec3(dot(rotation[0], turned), dot(rotation[1], turned), dot(rotation[2], turned));
}

ivec2 probeDirectionTexel(vec3 unitDirection, int side) {
    vec2 square = octahedralEncode(unitDirection);
    return clamp(ivec2(floor((square * 0.5 + 0.5) * float(side))), ivec2(0), ivec2(side - 1));
}

bool probesSeeEachOther(float meanDistance, float separation) {
    return meanDistance >= DDGI_SHARE_VISIBLE_SHARE * separation;
}

vec3 sharedProbeValue(vec3 own, vec3 neighbours[DDGI_SHARE_NEIGHBOURS], bool visible[DDGI_SHARE_NEIGHBOURS], float weight) {
    vec3 sum = own;
    float weights = 1.0;

    for (int neighbour = 0; neighbour < DDGI_SHARE_NEIGHBOURS; ++neighbour) {
        if (visible[neighbour]) {
            sum += neighbours[neighbour] * weight;
            weights += weight;
        }
    }

    return sum * (1.0 / weights);
}

bool probeNearLight(vec3 probe, vec3 light, float range, float reach) {
    return length(light - probe) < range + reach;
}

float irradianceRayWeight(vec3 texelDirection, vec3 rayDirection) {
    return max(0.0, dot(texelDirection, rayDirection));
}

float distanceRayWeight(vec3 texelDirection, vec3 rayDirection) {
    return pow(irradianceRayWeight(texelDirection, rayDirection), DDGI_DISTANCE_SHARPNESS);
}

float distanceForUpdate(float rayDistance, float maxDistance) {
    float distance = rayDistance < 0.0 ? -rayDistance * DDGI_BACK_FACE_DISTANCE_SHARE : rayDistance;
    return min(distance, maxDistance);
}

vec3 blendProbeValue(vec3 oldValue, vec3 newValue, float hysteresis) {
    return newValue + (oldValue - newValue) * hysteresis;
}

vec2 blendProbeValue(vec2 oldValue, vec2 newValue, float hysteresis) {
    return newValue + (oldValue - newValue) * hysteresis;
}

float probeLuminance(vec3 light) {
    return dot(light, vec3(BLOOM_LUMA_RED, BLOOM_LUMA_GREEN, BLOOM_LUMA_BLUE));
}

struct AdaptiveProbeTexel {
    vec3 stored;
    float history;
    vec3 recent;
    float noise;
};

float historyForShare(float keep) {
    return keep < 1.0 ? min(keep / (1.0 - keep), float(DDGI_MAX_HISTORY)) : float(DDGI_MAX_HISTORY);
}

AdaptiveProbeTexel adaptiveProbeBlend(AdaptiveProbeTexel texel, vec3 estimate, float longKeep, float windowKeep, float threshold) {
    AdaptiveProbeTexel next = texel;

    if (texel.history < 1.0) {
        next.recent = texel.stored;
        next.noise = 0.0;
        next.history = historyForShare(longKeep);
    } else {
        float deviation = probeLuminance(estimate) - probeLuminance(texel.recent);
        float variance = deviation * deviation + (texel.noise * texel.noise - deviation * deviation) * longKeep;
        next.noise = sqrt(variance);
    }

    next.recent = blendProbeValue(next.recent, estimate, windowKeep);
    next.stored = blendProbeValue(next.stored, estimate, min(longKeep, next.history / (next.history + 1.0)));

    float windowSpread = next.noise * sqrt((1.0 - windowKeep) / (1.0 + windowKeep));
    float band = threshold * windowSpread + DDGI_CHANGE_FLOOR_SHARE * probeLuminance(next.stored);

    if (abs(probeLuminance(next.recent) - probeLuminance(next.stored)) > band) {
        next.stored = next.recent;
        next.history = historyForShare(windowKeep);
    } else {
        next.history = min(next.history + 1.0, float(DDGI_MAX_HISTORY));
    }

    return next;
}

float eventProbeSharingWeight(float weight, float eventWeight, int framesSinceRestart, float longKeep) {
    float period = historyForShare(longKeep);

    if (framesSinceRestart < 0 || !(period > 0.0)) {
        return weight;
    }

    float faded = min(float(framesSinceRestart) / period, 1.0);
    return max(weight, eventWeight + (weight - eventWeight) * faded);
}

// A probe's scene event this frame (render/gpu/ddgi.h, GpuProbeRestart): the frames since its irradiance (x) and
// distances (y) were restarted, negative for none, and the share of each it keeps this frame.
struct ProbeRestart {
    ivec2 frames;
    vec2 keeps;
};

AdaptiveProbeTexel restartProbeTexel(AdaptiveProbeTexel texel, vec3 estimate, float keep) {
    vec3 stored = blendProbeValue(texel.stored, estimate, keep);
    return AdaptiveProbeTexel(stored, historyForShare(keep) + 1.0, stored, 0.0);
}

AdaptiveProbeTexel restartedProbeBlend(AdaptiveProbeTexel texel, vec3 estimate, int framesSinceRestart, float keep) {
    float frames = float(framesSinceRestart);
    float deviation = probeLuminance(estimate) - probeLuminance(texel.recent);
    float variance = deviation * deviation + (texel.noise * texel.noise - deviation * deviation) * ((frames - 1.0) / frames);
    vec3 stored = blendProbeValue(texel.stored, estimate, keep);
    return AdaptiveProbeTexel(stored, historyForShare(keep) + 1.0, stored, sqrt(variance));
}

AdaptiveProbeTexel adaptiveProbeUpdate(AdaptiveProbeTexel texel, vec3 estimate, float longKeep, float windowKeep, float threshold,
                                       int framesSinceRestart, float restartKeep) {
    if (framesSinceRestart == 0) {
        return restartProbeTexel(texel, estimate, restartKeep);
    }

    if (framesSinceRestart > 0 && float(framesSinceRestart) < historyForShare(windowKeep)) {
        return restartedProbeBlend(texel, estimate, framesSinceRestart, restartKeep);
    }

    return adaptiveProbeBlend(texel, estimate, longKeep, windowKeep, threshold);
}

vec3 probePosition(vec3 origin, vec3 spacing, ivec3 counts, int index) {
    ivec3 grid = ivec3(index % counts.x, (index / counts.x) % counts.y, index / (counts.x * counts.y));
    return origin + spacing * vec3(grid);
}

ivec2 probeTileOrigin(ivec3 counts, int index, int side) {
    int columns = counts.x * counts.y;
    return ivec2(index % columns, index / columns) * (side + 2);
}

vec2 probeAtlasUv(ivec3 counts, int index, vec3 direction, int side) {
    vec2 square = octahedralEncode(direction);
    ivec2 origin = probeTileOrigin(counts, index, side);
    vec2 atlasSize = vec2(counts.x * counts.y, counts.z) * float(side + 2);
    return (vec2(origin + 1) + (square * 0.5 + 0.5) * float(side)) / atlasSize;
}

#endif
