#include "render/gpu/ddgi.h"
#include "core/hash.h"
#include "render/gpu/bloom_shared.h"
#include "math/angles.h"
#include "math/mat4.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {
    // 1 for zero and up, -1 below: the octahedral fold needs a sign that is never 0.
    float signNotZero(float value) {
        return value >= 0.0f ? 1.0f : -1.0f;
    }

    // The golden ratio less one: the share of a turn between neighbouring Fibonacci points.
    constexpr float goldenFraction = 0.6180339887f;

    // The bordered square's side.
    int borderedSide(int side) {
        return side + 2;
    }

    // Probes' own turns are drawn from numbers this high, which no ray cycle (frame number / probeRayCycle) reaches.
    constexpr std::uint64_t probeTurnNumbers = std::uint64_t{1} << 62;

    // The frames whose running mean keeps this share (n / (n + 1) = keep), at most maxProbeHistory.
    float historyForShare(float keep) {
        return keep < 1.0f ? std::min(keep / (1.0f - keep), static_cast<float>(maxProbeHistory)) : static_cast<float>(maxProbeHistory);
    }

    // value mod count, never negative.
    int wrapped(int value, int count) {
        const int remainder = value % count;
        return remainder < 0 ? remainder + count : remainder;
    }
}

void visit(GlobalIlluminationSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Enabled", settings.enabled);
    visitor.field("Cascades", settings.probeCascades, {.min = 1, .max = DDGI_MAX_CASCADES});
    visitor.field("Probe spacing", settings.probeSpacing, {.min = 0.05f, .max = 8.0f, .sliderMax = 2.0f, .unit = "m (densest)"});
    visitor.field("Densest probes across", settings.densestProbesAcross, {.min = 4, .max = 61});
    visitor.field("Densest probes high", settings.densestProbesHigh, {.min = 4, .max = 41});
    visitor.field("Probes across", settings.cascadeProbesAcross, {.min = 4, .max = 41});
    visitor.field("Probes high", settings.cascadeProbesHigh, {.min = 4, .max = 41});
    visitor.field("Seed starting probes", settings.seedProbes);
    visitor.field("Rays per probe", settings.raysPerProbe, {.min = minRaysPerProbe, .max = maxRaysPerProbe});
    visitor.field("Probe updates per second", settings.probeUpdatesPerSecond, {.min = 0.0f, .max = 1000.0f, .sliderMax = 240.0f, .unit = "(0: every frame)"});
    visitor.field("Irradiance time constant", settings.irradianceTimeConstant, {.min = 0.0f, .max = 10.0f, .sliderMax = 10.0f, .unit = "s"});
    visitor.field("Distance time constant", settings.distanceTimeConstant, {.min = 0.0f, .max = 10.0f, .sliderMax = 4.0f, .unit = "s"});
    visitor.field("Adaptive memory", settings.adaptiveMemory);
    visitor.field("Change window", settings.changeWindowSeconds, {.min = 0.01f, .max = 2.0f, .sliderMax = 1.0f, .unit = "s"});
    visitor.field("Change threshold", settings.changeThreshold, {.min = 1.0f, .max = 20.0f, .sliderMax = 10.0f, .unit = "sd"});
    visitor.field("Scene events", settings.sceneEvents);
    visitor.field("Event probe sharing", settings.eventProbeSharing, {.min = 0.0f, .max = 1.0f});
    visitor.field("Event memory", settings.eventMemorySeconds, {.min = 0.0f, .max = 2.0f, .sliderMax = 0.5f, .unit = "s"});
    visitor.field("Event memory growth", settings.eventMemoryGrowth, {.min = 0.05f, .max = 1.0f});
    visitor.field("Event change threshold", settings.eventChangeThreshold, {.min = 0.0f, .max = 20.0f, .sliderMax = 10.0f, .unit = "sd"});
    visitor.field("Event check", settings.eventCheckSeconds, {.min = 0.0f, .max = 2.0f, .sliderMax = 1.0f, .unit = "s"});
    visitor.field("Probe sharing", settings.probeSharing, {.min = 0.0f, .max = 1.0f});
    visitor.field("Fast probe sharing", settings.fastProbeSharing, {.min = 0.0f, .max = 1.0f});
    visitor.field("Fast lights", settings.fastLights);
    visitor.field("Fast irradiance time constant", settings.fastIrradianceTimeConstant, {.min = 0.0f, .max = 10.0f, .sliderMax = 0.5f, .unit = "s"});
    visitor.field("Fast rays per probe", settings.fastRaysPerProbe, {.min = minRaysPerProbe, .max = maxRaysPerProbe});
    visitor.field("Fast blend frame cap", settings.fastBlendMaxFrameSeconds, {.min = 0.001f, .max = 1.0f, .sliderMax = 0.1f, .unit = "s"});
    visitor.field("Self-shadow bias", settings.selfShadowBias, {.min = 0.0f, .max = 2.0f, .sliderMax = 1.0f});
    visitor.field("Bounce energy", settings.bounceEnergy, {.min = 0.0f, .max = 1.0f});
    visitor.field("Lights per hit", settings.lightsPerHit, {.min = 0, .max = DDGI_MAX_HIT_LIGHTS});
    visitor.field("Probe relocation", settings.probeRelocation);
    visitor.field("Probe states", settings.probeStates);
    visitor.field("Wake distance", settings.wakeCells, {.min = 0.0f, .max = 4.0f, .unit = "cells"});
    visitor.field("Show probes", settings.showProbes);
    visitor.field("Shown probe cascade", settings.shownProbeCascade, {.min = 0, .max = DDGI_MAX_CASCADES - 1, .unit = "(0: densest)"});
    visitor.field("Probe ball radius", settings.probeBallRadius, {.min = 0.01f, .max = 1.0f, .sliderMax = 0.4f, .unit = "m"});
}

/*
* Dividing by |x| + |y| + |z| puts the direction on the octahedron; its upper half seen from above is the diamond
* |u| + |v| <= 1, and a point of the lower half is folded over the diamond's nearest edge into a corner triangle.
*/
Vec2 octahedralEncode(const Vec3& unitDirection) {
    const float sum = std::abs(unitDirection.x) + std::abs(unitDirection.y) + std::abs(unitDirection.z);
    Vec2 square{unitDirection.x / sum, unitDirection.y / sum};

    if (unitDirection.z < 0.0f) {
        square = Vec2{(1.0f - std::abs(square.y)) * signNotZero(square.x), (1.0f - std::abs(square.x)) * signNotZero(square.y)};
    }

    return square;
}

// The fold undone: inside the diamond z is what is left of 1; outside it z is negative and x, y unfold the same way.
Vec3 octahedralDecode(const Vec2& square) {
    Vec3 direction{square.x, square.y, 1.0f - std::abs(square.x) - std::abs(square.y)};

    if (direction.z < 0.0f) {
        const float x = direction.x;
        direction.x = (1.0f - std::abs(direction.y)) * signNotZero(x);
        direction.y = (1.0f - std::abs(x)) * signNotZero(direction.y);
    }

    return direction.normalized();
}

Vec3 probeTexelDirection(TexelCoord texel, int side) {
    const float size = static_cast<float>(side);
    return octahedralDecode(Vec2{(static_cast<float>(texel.x) + 0.5f) / size * 2.0f - 1.0f, (static_cast<float>(texel.y) + 0.5f) / size * 2.0f - 1.0f});
}

/*
* Crossing an edge of the square comes back in on the same edge mirrored about its middle, so an edge texel copies the
* inner texel beside it with the other coordinate mirrored (x becomes side + 1 - x); crossing two edges at a corner
* mirrors both, which lands on the opposite corner.
*/
TexelCoord probeBorderSource(TexelCoord bordered, int side) {
    const int last = side + 1;
    const bool onColumn = bordered.x == 0 || bordered.x == last;
    const bool onRow = bordered.y == 0 || bordered.y == last;

    if (onColumn && onRow) {
        return TexelCoord{bordered.x == 0 ? side : 1, bordered.y == 0 ? side : 1};
    }

    if (onColumn) {
        return TexelCoord{bordered.x == 0 ? 1 : side, last - bordered.y};
    }

    if (onRow) {
        return TexelCoord{last - bordered.x, bordered.y == 0 ? 1 : side};
    }

    return bordered;
}

int probeBorderTexelCount(int side) {
    return 4 * side + 4;
}

TexelCoord probeBorderTexel(int number, int side) {
    const int last = side + 1;

    if (number < side) {
        return TexelCoord{1 + number, 0};
    }

    if (number < 2 * side) {
        return TexelCoord{1 + number - side, last};
    }

    if (number < 3 * side) {
        return TexelCoord{0, 1 + number - 2 * side};
    }

    if (number < 4 * side) {
        return TexelCoord{last, 1 + number - 3 * side};
    }

    const int corner = number - 4 * side;
    return TexelCoord{(corner & 1) != 0 ? last : 0, (corner & 2) != 0 ? last : 0};
}

Vec3 sphericalFibonacci(int index, int count) {
    const float z = 1.0f - (2.0f * static_cast<float>(index) + 1.0f) / static_cast<float>(count);
    const float turns = static_cast<float>(index) * goldenFraction;
    const float angle = fullTurnRadians * (turns - std::floor(turns));
    const float radius = std::sqrt(std::max(0.0f, 1.0f - z * z));
    return Vec3{radius * std::cos(angle), radius * std::sin(angle), z};
}

/*
* Three uniform numbers from the frame number's mixed bits (21 bits each) make a uniform rotation: Shoemake's
* construction splits the unit quaternion's four components into two pairs whose shares of the length are sqrt(1 - u1)
* and sqrt(u1), each pair at a uniform angle.
*/
Quat randomProbeRotation(std::uint64_t frameNumber) {
    const std::uint64_t bits = mixBits(frameNumber);
    const float scale = 1.0f / static_cast<float>(1u << 21);
    const float u1 = static_cast<float>(bits & 0x1fffffu) * scale;
    const float u2 = static_cast<float>((bits >> 21) & 0x1fffffu) * scale;
    const float u3 = static_cast<float>((bits >> 42) & 0x1fffffu) * scale;

    const float first = std::sqrt(1.0f - u1);
    const float second = std::sqrt(u1);
    const float angle2 = fullTurnRadians * u2;
    const float angle3 = fullTurnRadians * u3;
    return Quat{first * std::sin(angle2), first * std::cos(angle2), second * std::sin(angle3), second * std::cos(angle3)};
}

ProbeRotationRows probeRotationRows(const Quat& rotation) {
    const Mat4 matrix = rotation.toMatrix();
    ProbeRotationRows rows;

    for (int row = 0; row < 3; ++row) {
        rows.rows[row] = Vec3{matrix.values[row][0], matrix.values[row][1], matrix.values[row][2]};
    }

    return rows;
}

ProbeRayFrame probeRayFrame(std::uint64_t frameNumber) {
    const std::uint64_t cycle = static_cast<std::uint64_t>(probeRayCycle);
    return ProbeRayFrame{randomProbeRotation(frameNumber / cycle), static_cast<int>(frameNumber % cycle)};
}

Vec3 probeRayDirection(int ray, int count, int phase, const ProbeRotationRows& rotation) {
    const Vec3 point = sphericalFibonacci(ray * probeRayCycle + phase, count * probeRayCycle);
    return Vec3{rotation.rows[0].dot(point), rotation.rows[1].dot(point), rotation.rows[2].dot(point)};
}

Vec3 probeRayDirection(int ray, int count, int phase, const ProbeRotationRows& rotation, const ProbeRotationRows& turn) {
    const Vec3 point = sphericalFibonacci(ray * probeRayCycle + phase, count * probeRayCycle);
    const Vec3 turned{turn.rows[0].dot(point), turn.rows[1].dot(point), turn.rows[2].dot(point)};
    return Vec3{rotation.rows[0].dot(turned), rotation.rows[1].dot(turned), rotation.rows[2].dot(turned)};
}

ProbeRotationRows probeRayTurn(int probe) {
    return probeRotationRows(randomProbeRotation(probeTurnNumbers + static_cast<std::uint64_t>(probe)));
}

std::vector<GpuProbeTurn> packProbeTurns(int probes, bool independent) {
    std::vector<GpuProbeTurn> turns(static_cast<std::size_t>(std::max(probes, 0)));

    for (int probe = 0; probe < probes; ++probe) {
        GpuProbeTurn& packed = turns[static_cast<std::size_t>(probe)];

        if (!independent) {
            for (int row = 0; row < 3; ++row) {
                packed.rows[row][row] = 1.0f;
            }

            continue;
        }

        const ProbeRotationRows turn = probeRayTurn(probe);

        for (int row = 0; row < 3; ++row) {
            packed.rows[row][0] = turn.rows[row].x;
            packed.rows[row][1] = turn.rows[row].y;
            packed.rows[row][2] = turn.rows[row].z;
        }
    }

    return turns;
}

TexelCoord probeDirectionTexel(const Vec3& unitDirection, int side) {
    const Vec2 square = octahedralEncode(unitDirection);
    const auto texel = [side](float coordinate) {
        return std::clamp(static_cast<int>(std::floor((coordinate * 0.5f + 0.5f) * static_cast<float>(side))), 0, side - 1);
    };
    return TexelCoord{texel(square.x), texel(square.y)};
}

bool probesSeeEachOther(float meanDistance, float separation) {
    return meanDistance >= static_cast<float>(DDGI_SHARE_VISIBLE_SHARE) * separation;
}

Vec3 sharedProbeValue(const Vec3& own, const std::array<Vec3, 6>& neighbours, const std::array<bool, 6>& visible, float weight) {
    Vec3 sum = own;
    float weights = 1.0f;

    for (std::size_t neighbour = 0; neighbour < neighbours.size(); ++neighbour) {
        if (visible[neighbour]) {
            sum = sum + neighbours[neighbour] * weight;
            weights += weight;
        }
    }

    return sum * (1.0f / weights);
}

GpuProbeShare packProbeShare(int probes, int cascades, float weight, float fastWeight, float eventWeight, float longKeep) {
    const ProbeAtlasGrid grid = probeAtlasGrid(probes);
    GpuProbeShare packed{};
    packed.counts[0] = grid.columns;
    packed.counts[1] = grid.rows;
    packed.counts[2] = probes;
    packed.counts[3] = cascades;
    packed.weight[0] = weight;
    packed.weight[1] = fastWeight;
    packed.weight[2] = eventWeight;
    packed.weight[3] = longKeep;
    return packed;
}

bool probeNearLight(const Vec3& probe, const Vec3& light, float range, float reach) {
    return (light - probe).length() < range + reach;
}

float irradianceRayWeight(const Vec3& texelDirection, const Vec3& rayDirection) {
    return std::max(0.0f, texelDirection.dot(rayDirection));
}

float distanceRayWeight(const Vec3& texelDirection, const Vec3& rayDirection) {
    return std::pow(irradianceRayWeight(texelDirection, rayDirection), probeDistanceSharpness);
}

float distanceForUpdate(float rayDistance, float maxDistance) {
    const float distance = rayDistance < 0.0f ? -rayDistance * backFaceDistanceShare : rayDistance;
    return std::min(distance, maxDistance);
}

float probeMaxDistance(const Vec3& spacing) {
    return maxDistanceDiagonals * spacing.length();
}

float hysteresisForTimeConstant(float frameSeconds, float timeConstant) {
    if (timeConstant <= 0.0f) {
        return 0.0f;
    }

    return std::exp(-frameSeconds / timeConstant);
}

Vec3 blendProbeValue(const Vec3& oldValue, const Vec3& newValue, float hysteresis) {
    return newValue + (oldValue - newValue) * hysteresis;
}

Vec2 blendProbeValue(const Vec2& oldValue, const Vec2& newValue, float hysteresis) {
    return newValue + (oldValue - newValue) * hysteresis;
}

float probeLuminance(const Vec3& light) {
    return light.x * static_cast<float>(BLOOM_LUMA_RED) + light.y * static_cast<float>(BLOOM_LUMA_GREEN) +
           light.z * static_cast<float>(BLOOM_LUMA_BLUE);
}

// The noise is kept as a standard deviation, not a variance, so a bright texel's square cannot overflow a half float.
AdaptiveProbeTexel adaptiveProbeBlend(const AdaptiveProbeTexel& texel, const Vec3& estimate, float longKeep, float windowKeep, float threshold) {
    AdaptiveProbeTexel next = texel;

    if (texel.history < 1.0f) {
        next.recent = texel.stored;
        next.noise = 0.0f;
        next.history = historyForShare(longKeep);
    } else {
        const float deviation = probeLuminance(estimate) - probeLuminance(texel.recent);
        const float variance = deviation * deviation + (texel.noise * texel.noise - deviation * deviation) * longKeep;
        next.noise = std::sqrt(variance);
    }

    next.recent = blendProbeValue(next.recent, estimate, windowKeep);
    next.stored = blendProbeValue(next.stored, estimate, std::min(longKeep, next.history / (next.history + 1.0f)));

    const float windowSpread = next.noise * std::sqrt((1.0f - windowKeep) / (1.0f + windowKeep));
    const float band = threshold * windowSpread + probeChangeFloorShare * probeLuminance(next.stored);

    if (std::abs(probeLuminance(next.recent) - probeLuminance(next.stored)) > band) {
        next.stored = next.recent;
        next.history = historyForShare(windowKeep);
    } else {
        next.history = std::min(next.history + 1.0f, static_cast<float>(maxProbeHistory));
    }

    return next;
}

AdaptiveProbeTexel restartProbeTexel(const Vec3& eventMean) {
    return AdaptiveProbeTexel{eventMean, -1.0f, eventMean, 0.0f};
}

// The n-th deviation since the restart joins the mean of their squares by 1 / n (n the texel's frames since).
AdaptiveProbeTexel restartedProbeBlend(const AdaptiveProbeTexel& texel, const Vec3& estimate, float keep) {
    const float frames = texel.history;
    const float deviation = probeLuminance(estimate) - probeLuminance(texel.recent);
    const float variance = deviation * deviation + (texel.noise * texel.noise - deviation * deviation) * ((frames - 1.0f) / frames);

    AdaptiveProbeTexel next;
    next.stored = blendProbeValue(texel.stored, estimate, keep);
    next.recent = next.stored;
    next.noise = std::sqrt(variance);
    next.history = std::min(frames + 1.0f, static_cast<float>(maxProbeHistory));
    return next;
}

// The band uses the noise measured before this frame, so a change does not widen the band it is tested against.
AdaptiveProbeTexel eventCheckBlend(const AdaptiveProbeTexel& texel, const Vec3& estimate, float longKeep, int framesSinceEvent, float eventThreshold) {
    const float frames = static_cast<float>(framesSinceEvent);
    const float deviation = probeLuminance(estimate) - probeLuminance(texel.recent);
    const float variance = deviation * deviation + (texel.noise * texel.noise - deviation * deviation) * longKeep;

    AdaptiveProbeTexel next;
    next.recent = blendProbeValue(texel.recent, estimate, frames / (frames + 1.0f));
    next.stored = blendProbeValue(texel.stored, estimate, std::min(longKeep, texel.history / (texel.history + 1.0f)));
    next.noise = std::sqrt(variance);
    next.history = std::min(texel.history + 1.0f, static_cast<float>(maxProbeHistory));

    const float band = eventThreshold * texel.noise / std::sqrt(frames + 1.0f) + probeChangeFloorShare * probeLuminance(next.stored);

    if (std::abs(probeLuminance(next.recent) - probeLuminance(next.stored)) > band) {
        return restartProbeTexel(next.recent);
    }

    return next;
}

AdaptiveProbeTexel adaptiveProbeUpdate(const AdaptiveProbeTexel& texel, const Vec3& estimate, float longKeep, float windowKeep, float threshold,
                                       int framesSinceEvent, const ProbeEventCheck& check) {
    AdaptiveProbeTexel own = texel;
    own.history = std::abs(texel.history);

    if (texel.history < 0.0f && framesSinceEvent == 0) {
        return restartProbeTexel(estimate);
    }

    if (texel.history < 0.0f && framesSinceEvent > 0) {
        const float keep = restartedProbeKeep(longKeep, 0.0f, check.growth, own.history);
        AdaptiveProbeTexel next = own.history < historyForShare(windowKeep) ? restartedProbeBlend(own, estimate, keep)
                                                                            : adaptiveProbeBlend(own, estimate, keep, windowKeep, threshold);
        next.history = -next.history;
        return next;
    }

    if (framesSinceEvent >= 0 && static_cast<float>(framesSinceEvent) < check.frames) {
        return eventCheckBlend(own, estimate, longKeep, framesSinceEvent, check.threshold);
    }

    return adaptiveProbeBlend(own, estimate, longKeep, windowKeep, threshold);
}

float eventCheckFrames(float checkSeconds, float frameSeconds) {
    return frameSeconds > 0.0f ? std::max(checkSeconds / frameSeconds, minEventCheckFrames) : minEventCheckFrames;
}

float restartedProbeKeep(float longKeep, float shortKeep, float growth, float frames) {
    if (frames < 0.0f) {
        return longKeep;
    }

    const float grown = frames == 0.0f ? 0.0f : std::exp(-1.0f / (growth * frames));
    return std::min(longKeep, std::max(shortKeep, grown));
}

float eventProbeSharingWeight(float weight, float eventWeight, int framesSinceRestart, float longKeep) {
    const float period = historyForShare(longKeep);

    if (framesSinceRestart < 0 || !(period > 0.0f)) {
        return weight;
    }

    const float faded = std::min(static_cast<float>(framesSinceRestart) / period, 1.0f);
    return std::max(weight, eventWeight + (weight - eventWeight) * faded);
}

void validateProbeVolume(const ProbeVolume& volume) {
    const ProbeCounts& counts = volume.counts;

    if (counts.x < 1 || counts.y < 1 || counts.z < 1) {
        throw std::invalid_argument("A probe volume needs at least one probe along each axis");
    }

    if (!(volume.spacing.x > 0.0f && volume.spacing.y > 0.0f && volume.spacing.z > 0.0f)) {
        throw std::invalid_argument("A probe volume's spacing must be positive along each axis");
    }

    // Counted in 64 bits, so huge counts cannot wrap round to something small.
    const long long probes = static_cast<long long>(counts.x) * counts.y * counts.z;

    if (probes > maxProbesPerVolume) {
        throw std::invalid_argument("A probe volume may hold at most " + std::to_string(maxProbesPerVolume) + " probes");
    }
}

int probeCount(const ProbeCounts& counts) {
    return counts.x * counts.y * counts.z;
}

int probeIndex(const ProbeCounts& counts, int x, int y, int z) {
    return x + counts.x * (y + counts.y * z);
}

int probeSlot(const ProbeVolume& volume, const ProbeGridCoord& grid) {
    const ProbeCounts& counts = volume.counts;
    return probeIndex(counts, wrapped(grid.x + volume.scroll.x, counts.x), wrapped(grid.y + volume.scroll.y, counts.y),
                      wrapped(grid.z + volume.scroll.z, counts.z));
}

ProbeGridCoord probeGridCoord(const ProbeVolume& volume, int slot) {
    const ProbeCounts& counts = volume.counts;
    const int x = slot % counts.x;
    const int y = (slot / counts.x) % counts.y;
    const int z = slot / (counts.x * counts.y);
    return ProbeGridCoord{wrapped(x - volume.scroll.x, counts.x), wrapped(y - volume.scroll.y, counts.y), wrapped(z - volume.scroll.z, counts.z)};
}

Vec3 probePosition(const ProbeVolume& volume, int slot) {
    const ProbeGridCoord grid = probeGridCoord(volume, slot);
    return volume.origin + Vec3{volume.spacing.x * static_cast<float>(grid.x), volume.spacing.y * static_cast<float>(grid.y),
                                volume.spacing.z * static_cast<float>(grid.z)};
}

/*
* The smallest number of columns whose square holds every probe, then as many rows as they need: as near square as
* whole rows allow, so the most probes one dispatch holds (65,535: 256 x 256 squares of 18 texels, 4,608 a side) stay
* far within the largest image side.
*/
ProbeAtlasGrid probeAtlasGrid(int probes) {
    if (probes < 1) {
        return ProbeAtlasGrid{};
    }

    int columns = static_cast<int>(std::sqrt(static_cast<double>(probes)));

    while (columns * columns < probes) {
        ++columns;
    }

    return ProbeAtlasGrid{columns, (probes + columns - 1) / columns};
}

ProbeAtlasSize probeAtlasSize(const ProbeAtlasGrid& grid, int side) {
    return ProbeAtlasSize{grid.columns * borderedSide(side), grid.rows * borderedSide(side)};
}

TexelCoord probeTileOrigin(const ProbeAtlasGrid& grid, int index, int side) {
    return TexelCoord{(index % grid.columns) * borderedSide(side), (index / grid.columns) * borderedSide(side)};
}

// The direction's place in the inner square (0..side texels from its first inner texel), then in the whole atlas.
Vec2 probeAtlasUv(const ProbeAtlasGrid& grid, int index, const Vec3& direction, int side) {
    const Vec2 square = octahedralEncode(direction);
    const TexelCoord origin = probeTileOrigin(grid, index, side);
    const ProbeAtlasSize size = probeAtlasSize(grid, side);
    const float inner = static_cast<float>(side);
    return Vec2{(static_cast<float>(origin.x + 1) + (square.x * 0.5f + 0.5f) * inner) / static_cast<float>(size.width),
                (static_cast<float>(origin.y + 1) + (square.y * 0.5f + 0.5f) * inner) / static_cast<float>(size.height)};
}

int probeRayStride(const ProbeFrame& frame) {
    return std::max(frame.raysPerProbe, frame.fastRaysPerProbe);
}

GpuProbeVolume packProbeVolume(int probes, int cascades, const ProbeFrame& frame) {
    const ProbeAtlasGrid grid = probeAtlasGrid(probes);
    GpuProbeVolume packed{};
    packed.counts[0] = grid.columns;
    packed.counts[1] = grid.rows;
    packed.counts[3] = probeRayStride(frame);
    packed.cascades[0] = cascades;
    packed.cascades[1] = probes;
    packed.keeps[0] = frame.fastIrradianceHysteresis;

    const ProbeRotationRows rows = probeRotationRows(frame.rotation);

    for (int row = 0; row < 3; ++row) {
        packed.rotation[row][0] = rows.rows[row].x;
        packed.rotation[row][1] = rows.rows[row].y;
        packed.rotation[row][2] = rows.rows[row].z;
    }

    packed.rotation[0][3] = static_cast<float>(frame.raysPerProbe);
    packed.rotation[1][3] = static_cast<float>(frame.fastRaysPerProbe);
    packed.rotation[2][3] = frame.windowHysteresis;
    packed.sky[0] = frame.sky.x;
    packed.sky[1] = frame.sky.y;
    packed.sky[2] = frame.sky.z;
    packed.sky[3] = frame.changeThreshold;
    packed.hysteresis[0] = frame.irradianceHysteresis;
    packed.hysteresis[1] = frame.distanceHysteresis;
    packed.hysteresis[2] = frame.ballRadius;
    packed.hysteresis[3] = static_cast<float>(frame.rayPhase);
    return packed;
}

int probeRayDivisor(int currentDivisor, float framesPerSecond, float updatesPerSecond, int raysPerProbe) {
    if (!(updatesPerSecond > 0.0f) || !(framesPerSecond > 0.0f)) {
        return 1;
    }

    int largest = 1;

    while (raysPerProbe / (largest * 2) >= minRaysPerProbe) {
        largest *= 2;
    }

    const float budgets = framesPerSecond / updatesPerSecond;
    int divisor = std::clamp(currentDivisor, 1, largest);

    while (divisor * 2 <= largest && budgets >= static_cast<float>(divisor * 2) * (1.0f - rayBudgetStepUpShare)) {
        divisor *= 2;
    }

    while (divisor > 1 && budgets < static_cast<float>(divisor) * (1.0f - rayBudgetStepDownShare)) {
        divisor /= 2;
    }

    return divisor;
}

/*
* The fast blend counts a frame as at most fastBlendMaxFrameSeconds. The ray budget reads the frame time smoothed by the
* same exponential blend as the probes, so the ladder follows the frame rate, not single frames.
*/
ProbeFrame scheduleProbeFrame(ProbeSchedule& schedule, const GlobalIlluminationSettings& settings, float frameSeconds, bool fastLights, bool fresh) {
    const float smoothing = schedule.smoothedFrameSeconds > 0.0f ? hysteresisForTimeConstant(frameSeconds, rayBudgetSmoothingSeconds) : 0.0f;
    schedule.smoothedFrameSeconds = frameSeconds + (schedule.smoothedFrameSeconds - frameSeconds) * smoothing;

    const int stillRays = std::clamp(settings.raysPerProbe, minRaysPerProbe, maxRaysPerProbe);
    const float framesPerSecond = schedule.smoothedFrameSeconds > 0.0f ? 1.0f / schedule.smoothedFrameSeconds : 0.0f;
    schedule.rayDivisor = probeRayDivisor(schedule.rayDivisor, framesPerSecond, settings.probeUpdatesPerSecond, stillRays);

    ProbeFrame frame;
    frame.raysPerProbe = stillRays / schedule.rayDivisor;
    frame.fastRaysPerProbe = fastLights ? std::clamp(settings.fastRaysPerProbe, minRaysPerProbe, maxRaysPerProbe) : frame.raysPerProbe;

    const ProbeRayFrame rays = probeRayFrame(schedule.frameNumber++);
    frame.rotation = rays.rotation;
    frame.rayPhase = rays.phase;

    const float fastFrameSeconds = std::min(frameSeconds, settings.fastBlendMaxFrameSeconds);
    frame.irradianceHysteresis = fresh ? 0.0f : hysteresisForTimeConstant(frameSeconds, settings.irradianceTimeConstant);
    frame.fastIrradianceHysteresis = fresh ? 0.0f : hysteresisForTimeConstant(fastFrameSeconds, settings.fastIrradianceTimeConstant);
    frame.distanceHysteresis = fresh ? 0.0f : hysteresisForTimeConstant(frameSeconds, settings.distanceTimeConstant);
    frame.windowHysteresis = fresh ? 0.0f : hysteresisForTimeConstant(frameSeconds, settings.changeWindowSeconds);
    frame.restartHysteresis = fresh ? 0.0f : hysteresisForTimeConstant(frameSeconds, settings.eventMemorySeconds);
    frame.restartGrowth = settings.eventMemoryGrowth;
    frame.restartThreshold = settings.eventChangeThreshold;
    frame.restartCheckFrames = fresh ? 0.0f : eventCheckFrames(settings.eventCheckSeconds, frameSeconds);
    frame.changeThreshold = settings.adaptiveMemory ? settings.changeThreshold : 0.0f;
    frame.ballRadius = settings.probeBallRadius;
    return frame;
}

std::int32_t advanceRestartFrames(std::int32_t frames, float keep, const ProbeFrame& frame) {
    if (frames < 0) {
        return frames;
    }

    const std::int32_t next = frames + 1;
    const float sinceCheck = static_cast<float>(next) - std::ceil(frame.restartCheckFrames);

    if (sinceCheck < 0.0f) {
        return next;
    }

    return restartedProbeKeep(keep, frame.restartHysteresis, frame.restartGrowth, sinceCheck) >= keep ? -1 : next;
}

void advanceProbeRestarts(std::vector<GpuProbeRestart>& restarts, std::span<const ProbeVolume> cascades, std::span<const ProbeEvent> events,
                          const ProbeFrame& frame, std::span<const int> restarted, std::span<const ProbeSeed> seeded) {
    std::size_t probes = 0;

    for (const ProbeVolume& volume : cascades) {
        probes += static_cast<std::size_t>(probeCount(volume.counts));
    }

    if (restarts.size() != probes) {
        restarts.assign(probes, GpuProbeRestart{});
    }

    for (GpuProbeRestart& restart : restarts) {
        restart.irradianceFrames = advanceRestartFrames(restart.irradianceFrames, frame.irradianceHysteresis, frame);
        restart.distanceFrames = advanceRestartFrames(restart.distanceFrames, frame.distanceHysteresis, frame);
        restart.seed = -1;
    }

    const bool keepsIrradiance = frame.irradianceHysteresis > 0.0f;
    const bool keepsDistances = frame.distanceHysteresis > 0.0f;

    for (const ProbeEvent& event : events) {
        std::size_t first = 0;

        for (const ProbeVolume& volume : cascades) {
            const int count = probeCount(volume.counts);

            for (int slot = 0; slot < count; ++slot) {
                if (!event.everywhere && !event.region.contains(probePosition(volume, slot))) {
                    continue;
                }

                GpuProbeRestart& restart = restarts[first + static_cast<std::size_t>(slot)];

                if (keepsIrradiance) {
                    restart.irradianceFrames = 0;
                }

                if (keepsDistances && event.kind == ProbeEventKind::GeometryChange) {
                    restart.distanceFrames = 0;
                }
            }

            first += static_cast<std::size_t>(count);
        }
    }

    // Their stored light belongs to another time or place, so none of it is kept and no texel checks it first.
    for (const int probe : restarted) {
        GpuProbeRestart& restart = restarts.at(static_cast<std::size_t>(probe));
        restart.irradianceFrames = 0;
        restart.distanceFrames = 0;
    }

    // A seed stands for its first frames: its restart starts that many frames on.
    for (const ProbeSeed& seed : seeded) {
        GpuProbeRestart& restart = restarts.at(static_cast<std::size_t>(seed.probe));
        restart.irradianceFrames = static_cast<std::int32_t>(probeSeedFrames);
        restart.seed = seed.slot;
    }

    for (GpuProbeRestart& restart : restarts) {
        restart.irradianceKeep = restartedProbeKeep(frame.irradianceHysteresis, frame.restartHysteresis, frame.restartGrowth,
                                                    static_cast<float>(restart.irradianceFrames));
        restart.distanceKeep = restartedProbeKeep(frame.distanceHysteresis, frame.restartHysteresis, frame.restartGrowth,
                                                  static_cast<float>(restart.distanceFrames));
        restart.check = ProbeEventCheck{frame.restartThreshold, frame.restartGrowth, frame.restartCheckFrames};
        restart.fastKeep = restartedProbeKeep(frame.fastIrradianceHysteresis, 0.0f, frame.restartGrowth, static_cast<float>(restart.irradianceFrames));
    }

    for (const int probe : restarted) {
        GpuProbeRestart& restart = restarts.at(static_cast<std::size_t>(probe));
        restart.distanceKeep = 0.0f;
        restart.check.threshold = 0.0f;

        if (restart.seed < 0) {
            restart.irradianceKeep = 0.0f;
            restart.fastKeep = 0.0f;
        }
    }
}

void advanceProbeRestarts(std::vector<GpuProbeRestart>& restarts, const ProbeVolume& volume, std::span<const ProbeEvent> events, const ProbeFrame& frame,
                          std::span<const int> restarted, std::span<const ProbeSeed> seeded) {
    advanceProbeRestarts(restarts, std::span<const ProbeVolume>{&volume, 1}, events, frame, restarted, seeded);
}

AdaptiveProbeTexel seededProbeTexel(const Vec3& seed) {
    return AdaptiveProbeTexel{seed, -probeSeedFrames, seed, 0.0f};
}
