#ifndef DDGI_GLSL
#define DDGI_GLSL

// Global illumination's light probes (DDGI): the octahedral images and their borders, the rays' directions, the
// update's weights and blending, and where each probe lives in the atlases. Pure functions, mirrored by
// render/gpu/ddgi.h, which explains each; the GPU tests check them against it.

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

// rotation: the frame's rotation matrix's rows.
vec3 probeRayDirection(int ray, int count, vec3 rotation[3]) {
    vec3 point = sphericalFibonacci(ray, count);
    return vec3(dot(rotation[0], point), dot(rotation[1], point), dot(rotation[2], point));
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

vec3 encodeIrradiance(vec3 irradiance) {
    return pow(irradiance, vec3(1.0 / DDGI_IRRADIANCE_POWER));
}

vec3 decodeIrradiance(vec3 encoded) {
    return pow(encoded, vec3(DDGI_IRRADIANCE_POWER));
}

float irradianceTexelHysteresis(float hysteresis, vec3 oldEncoded, vec3 newEncoded) {
    vec3 change = abs(newEncoded - oldEncoded);
    float largest = max(change.x, max(change.y, change.z));

    if (largest > DDGI_RESET_THRESHOLD) {
        return 0.0;
    }

    if (largest > DDGI_CHANGE_THRESHOLD) {
        return max(0.0, hysteresis - DDGI_CHANGE_HYSTERESIS_DROP);
    }

    return hysteresis;
}

vec3 blendProbeValue(vec3 oldValue, vec3 newValue, float hysteresis) {
    return newValue + (oldValue - newValue) * hysteresis;
}

vec2 blendProbeValue(vec2 oldValue, vec2 newValue, float hysteresis) {
    return newValue + (oldValue - newValue) * hysteresis;
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
