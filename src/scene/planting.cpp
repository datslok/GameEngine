#include "scene/planting.h"

#include "core/hash.h"

#include <algorithm>
#include <cmath>

float plantingRandom(std::uint64_t seed, std::uint64_t index) {
    return static_cast<float>(mixBits(seed ^ mixBits(index + goldenGamma)) >> 40) * plantingUnitStep;
}

std::uint64_t plantingCellKey(int column, int row) {
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(column)) << 32) | static_cast<std::uint32_t>(row);
}

float plantingThreshold(int column, int row) {
    const std::uint32_t bits = static_cast<std::uint32_t>(column) * PLANTING_DITHER_STEP_COLUMN + static_cast<std::uint32_t>(row) * PLANTING_DITHER_STEP_ROW;
    return static_cast<float>(bits >> 8) * plantingUnitStep;
}

float plantingBetween(float a, float b, float share) {
    return a + (b - a) * share;
}

float plantingCoordinate(float position, float origin, float inverseTexel) {
    return (position - origin) * inverseTexel;
}

float plantingPathShare(float distance, float near, float far, float farShare, float inverseSpan) {
    if (distance <= near) {
        return 1.0f;
    }

    if (distance >= far) {
        return farShare;
    }

    return 1.0f + (farShare - 1.0f) * ((distance - near) * inverseSpan);
}

float plantingShare(const std::array<float, 3>& weights, const std::array<float, 3>& sharePerWeight, float gap, float gapStep, float pathShare) {
    const float perCell = weights[0] * sharePerWeight[0] + weights[1] * sharePerWeight[1] + weights[2] * sharePerWeight[2];

    if (perCell <= 0.0f) {
        return 0.0f;
    }

    const float gapped = 1.0f + gapStep * gap;
    return std::clamp(perCell * gapped * pathShare, 0.0f, 1.0f);
}

bool plantingTaken(float threshold, float below, float share) {
    return threshold >= below && threshold < below + std::min(share, 1.0f - below);
}

Vec2 plantingJitteredPlace(int column, int row, float footprint, float jitterMetres, float drawX, float drawZ) {
    const float middleX = (static_cast<float>(column) + 0.5f) * footprint;
    const float middleZ = (static_cast<float>(row) + 0.5f) * footprint;
    return Vec2{middleX + (drawX - 0.5f) * jitterMetres, middleZ + (drawZ - 0.5f) * jitterMetres};
}

float plantingScale(float least, float range, float draw) {
    return least + range * draw;
}

float plantingTurn(float draw) {
    return plantingFullTurn * draw;
}

int plantingUniformKind(float draw, int count) {
    return std::min(static_cast<int>(draw * static_cast<float>(count)), count - 1);
}

int plantingSharedKind(float draw, const std::array<float, plantingMostSharedKinds - 1>& cumulative, int count) {
    int kind = 0;

    while (kind < count - 1 && draw >= cumulative[static_cast<std::size_t>(kind)]) {
        ++kind;
    }

    return kind;
}

bool plantingWithin(const Vec2& place, const Vec2& other, float radius) {
    const float dx = place.x - other.x;
    const float dz = place.y - other.y;
    return dx * dx + dz * dz < radius * radius;
}

float plantingStored(float stored, float step, float offset) {
    return stored * step - offset;
}

float plantingPathDistance(float blended) {
    return std::fabs(blended) * plantingCentimetre;
}

bool plantingNearSegment(const Vec2& place, const Vec2& start, const Vec2& along, float inverseLengthSquared, float radius) {
    const float offX = place.x - start.x;
    const float offZ = place.y - start.y;
    const float share = std::clamp((offX * along.x + offZ * along.y) * inverseLengthSquared, 0.0f, 1.0f);
    return plantingWithin(place, Vec2{start.x + along.x * share, start.y + along.y * share}, radius);
}

Vec2 plantingRingPoint(const Vec2& place, float reach, int k) {
    constexpr float d = static_cast<float>(PLANTING_RING_DIAGONAL);
    static const std::array<Vec2, PLANTING_RING_POINTS> directions{
        Vec2{1.0f, 0.0f}, Vec2{d, d}, Vec2{0.0f, 1.0f}, Vec2{-d, d}, Vec2{-1.0f, 0.0f}, Vec2{-d, -d}, Vec2{0.0f, -1.0f}, Vec2{d, -d}
    };
    const Vec2 direction = directions[static_cast<std::size_t>(k % PLANTING_RING_POINTS)];
    return Vec2{place.x + reach * direction.x, place.y + reach * direction.y};
}

float plantingTop(float footHeight, float centrePerScale, float scale, float reach) {
    return footHeight + centrePerScale * scale + reach;
}

bool plantingFits(float clearance, float reach, float top, float corridor, float cap) {
    return clearance >= reach && top + corridor <= cap;
}

bool plantingGivesWay(const Vec2& place, float reach, float threshold, const Vec2& other, float otherReach, float otherThreshold,
                      bool otherEarlier, float spacingShare) {
    const bool first = otherThreshold < threshold || (otherThreshold == threshold && otherEarlier);
    return first && plantingWithin(place, other, spacingShare * std::max(reach, otherReach));
}
