// Planting from maps: the GPU's copy of scene/planting.cpp, tested bit for bit against it (tests/gpu). Adds, multiplies,
// compares and whole numbers only, every result precise (no multiply and add fused into one rounding), so the GPU
// plants exactly what the CPU does. A shader including it turns on GL_EXT_shader_explicit_arithmetic_types_int64.
// No buffer reads here: the maps are read by whoever includes it.

#ifndef PLANTING_GLSL
#define PLANTING_GLSL

#include "hash64.glsl"
#include "scene/planting_shared.h"

float plantingRandom(uint64_t seed, uint64_t index) {
    precise float unit = float(uint(mixBits(seed ^ mixBits(index + HASH_GOLDEN_GAMMA)) >> 40)) * float(PLANTING_UNIT_STEP);
    return unit;
}

uint64_t plantingCellKey(int column, int row) {
    return (uint64_t(uint(column)) << 32) | uint64_t(uint(row));
}

float plantingThreshold(int column, int row) {
    uint bits = uint(column) * PLANTING_DITHER_STEP_COLUMN + uint(row) * PLANTING_DITHER_STEP_ROW;
    precise float threshold = float(bits >> 8) * float(PLANTING_UNIT_STEP);
    return threshold;
}

float plantingBetween(float a, float b, float share) {
    precise float between = a + (b - a) * share;
    return between;
}

float plantingCoordinate(float position, float origin, float inverseTexel) {
    precise float coordinate = (position - origin) * inverseTexel;
    return coordinate;
}

float plantingPathShare(float distance, float near, float far, float farShare, float inverseSpan) {
    if (distance <= near) {
        return 1.0;
    }

    if (distance >= far) {
        return farShare;
    }

    precise float share = 1.0 + (farShare - 1.0) * ((distance - near) * inverseSpan);
    return share;
}

float plantingShare(vec3 weights, vec3 sharePerWeight, float gap, float gapStep, float pathShare) {
    precise float perCell = weights.x * sharePerWeight.x + weights.y * sharePerWeight.y + weights.z * sharePerWeight.z;

    if (perCell <= 0.0) {
        return 0.0;
    }

    precise float gapped = 1.0 + gapStep * gap;
    precise float share = clamp(perCell * gapped * pathShare, 0.0, 1.0);
    return share;
}

bool plantingTaken(float threshold, float below, float share) {
    precise float above = below + min(share, 1.0 - below);
    return threshold >= below && threshold < above;
}

vec2 plantingJitteredPlace(int column, int row, float footprint, float jitterMetres, float drawX, float drawZ) {
    precise float middleX = (float(column) + 0.5) * footprint;
    precise float middleZ = (float(row) + 0.5) * footprint;
    precise vec2 place = vec2(middleX + (drawX - 0.5) * jitterMetres, middleZ + (drawZ - 0.5) * jitterMetres);
    return place;
}

float plantingScale(float least, float range, float draw) {
    precise float scale = least + range * draw;
    return scale;
}

float plantingTurn(float draw) {
    precise float turn = float(PLANTING_FULL_TURN) * draw;
    return turn;
}

int plantingUniformKind(float draw, int count) {
    precise float scaled = draw * float(count);
    return min(int(scaled), count - 1);
}

// cumulative: the shares up to each kind but the last (PLANTING_MOST_SHARED_KINDS - 1 of them).
int plantingSharedKind(float draw, vec4 cumulative, int count) {
    int kind = 0;

    while (kind < count - 1 && draw >= cumulative[kind]) {
        ++kind;
    }

    return kind;
}

bool plantingWithin(vec2 place, vec2 other, float radius) {
    precise float dx = place.x - other.x;
    precise float dz = place.y - other.y;
    precise float apart = dx * dx + dz * dz;
    precise float most = radius * radius;
    return apart < most;
}

float plantingStored(float stored, float step, float offset) {
    precise float metres = stored * step - offset;
    return metres;
}

float plantingPathDistance(float blended) {
    precise float metres = abs(blended) * float(PLANTING_CENTIMETRE);
    return metres;
}

bool plantingNearSegment(vec2 place, vec2 start, vec2 along, float inverseLengthSquared, float radius) {
    precise float offX = place.x - start.x;
    precise float offZ = place.y - start.y;
    precise float share = clamp((offX * along.x + offZ * along.y) * inverseLengthSquared, 0.0, 1.0);
    precise vec2 nearest = vec2(start.x + along.x * share, start.y + along.y * share);
    return plantingWithin(place, nearest, radius);
}

vec2 plantingRingPoint(vec2 place, float reach, int k) {
    const float d = float(PLANTING_RING_DIAGONAL);
    const vec2 directions[PLANTING_RING_POINTS] = vec2[](vec2(1.0, 0.0), vec2(d, d), vec2(0.0, 1.0), vec2(-d, d), vec2(-1.0, 0.0),
                                                         vec2(-d, -d), vec2(0.0, -1.0), vec2(d, -d));
    vec2 direction = directions[k % PLANTING_RING_POINTS];
    precise vec2 point = vec2(place.x + reach * direction.x, place.y + reach * direction.y);
    return point;
}

float plantingTop(float footHeight, float centrePerScale, float scale, float reach) {
    precise float top = footHeight + centrePerScale * scale + reach;
    return top;
}

bool plantingFits(float clearance, float reach, float top, float corridor, float cap) {
    precise float highest = top + corridor;
    return clearance >= reach && highest <= cap;
}

bool plantingGivesWay(vec2 place, float reach, float threshold, vec2 other, float otherReach, float otherThreshold, bool otherEarlier,
                      float spacingShare) {
    bool first = otherThreshold < threshold || (otherThreshold == threshold && otherEarlier);
    precise float spacing = spacingShare * max(reach, otherReach);
    return first && plantingWithin(place, other, spacing);
}

#endif
