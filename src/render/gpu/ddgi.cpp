#include "render/gpu/ddgi.h"
#include "core/hash.h"
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

    // Squares across one row of an atlas: one layer of the grid.
    int atlasColumns(const ProbeCounts& counts) {
        return counts.x * counts.y;
    }
}

void visit(GlobalIlluminationSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Enabled", settings.enabled);
    visitor.field("Rays per probe", settings.raysPerProbe, {.min = minRaysPerProbe, .max = maxRaysPerProbe});
    visitor.field("Irradiance time constant", settings.irradianceTimeConstant, {.min = 0.0f, .max = 10.0f, .sliderMax = 2.0f, .unit = "s"});
    visitor.field("Distance time constant", settings.distanceTimeConstant, {.min = 0.0f, .max = 10.0f, .sliderMax = 4.0f, .unit = "s"});
    visitor.field("Self-shadow bias", settings.selfShadowBias, {.min = 0.0f, .max = 2.0f, .sliderMax = 1.0f});
    visitor.field("Bounce energy", settings.bounceEnergy, {.min = 0.0f, .max = 1.0f});
    visitor.field("Lights per hit", settings.lightsPerHit, {.min = 0, .max = DDGI_MAX_HIT_LIGHTS});
    visitor.field("Show probes", settings.showProbes);
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

Vec3 probeRayDirection(int ray, int count, const ProbeRotationRows& rotation) {
    const Vec3 point = sphericalFibonacci(ray, count);
    return Vec3{rotation.rows[0].dot(point), rotation.rows[1].dot(point), rotation.rows[2].dot(point)};
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

    const long long widest = static_cast<long long>(atlasColumns(counts)) * borderedSide(probeDistanceTexels);
    const long long tallest = static_cast<long long>(counts.z) * borderedSide(probeDistanceTexels);

    if (widest > maxProbeAtlasSide || tallest > maxProbeAtlasSide) {
        throw std::invalid_argument("A probe volume's atlases would be larger than " + std::to_string(maxProbeAtlasSide) + " texels");
    }
}

int probeCount(const ProbeCounts& counts) {
    return counts.x * counts.y * counts.z;
}

int probeIndex(const ProbeCounts& counts, int x, int y, int z) {
    return x + counts.x * (y + counts.y * z);
}

Vec3 probePosition(const ProbeVolume& volume, int index) {
    const int x = index % volume.counts.x;
    const int y = (index / volume.counts.x) % volume.counts.y;
    const int z = index / (volume.counts.x * volume.counts.y);
    return volume.origin + Vec3{volume.spacing.x * static_cast<float>(x), volume.spacing.y * static_cast<float>(y),
                                volume.spacing.z * static_cast<float>(z)};
}

ProbeAtlasSize probeAtlasSize(const ProbeCounts& counts, int side) {
    return ProbeAtlasSize{atlasColumns(counts) * borderedSide(side), counts.z * borderedSide(side)};
}

TexelCoord probeTileOrigin(const ProbeCounts& counts, int index, int side) {
    const int columns = atlasColumns(counts);
    return TexelCoord{(index % columns) * borderedSide(side), (index / columns) * borderedSide(side)};
}

// The direction's place in the inner square (0..side texels from its first inner texel), then in the whole atlas.
Vec2 probeAtlasUv(const ProbeCounts& counts, int index, const Vec3& direction, int side) {
    const Vec2 square = octahedralEncode(direction);
    const TexelCoord origin = probeTileOrigin(counts, index, side);
    const ProbeAtlasSize size = probeAtlasSize(counts, side);
    const float inner = static_cast<float>(side);
    return Vec2{(static_cast<float>(origin.x + 1) + (square.x * 0.5f + 0.5f) * inner) / static_cast<float>(size.width),
                (static_cast<float>(origin.y + 1) + (square.y * 0.5f + 0.5f) * inner) / static_cast<float>(size.height)};
}

GpuProbeVolume packProbeVolume(const ProbeVolume& volume, const ProbeFrame& frame) {
    GpuProbeVolume packed{};
    packed.origin[0] = volume.origin.x;
    packed.origin[1] = volume.origin.y;
    packed.origin[2] = volume.origin.z;
    packed.spacing[0] = volume.spacing.x;
    packed.spacing[1] = volume.spacing.y;
    packed.spacing[2] = volume.spacing.z;
    packed.spacing[3] = probeMaxDistance(volume.spacing);
    packed.counts[0] = volume.counts.x;
    packed.counts[1] = volume.counts.y;
    packed.counts[2] = volume.counts.z;
    packed.counts[3] = frame.raysPerProbe;

    const ProbeRotationRows rows = probeRotationRows(frame.rotation);

    for (int row = 0; row < 3; ++row) {
        packed.rotation[row][0] = rows.rows[row].x;
        packed.rotation[row][1] = rows.rows[row].y;
        packed.rotation[row][2] = rows.rows[row].z;
    }

    packed.sky[0] = frame.sky.x;
    packed.sky[1] = frame.sky.y;
    packed.sky[2] = frame.sky.z;
    packed.hysteresis[0] = frame.irradianceHysteresis;
    packed.hysteresis[1] = frame.distanceHysteresis;
    packed.hysteresis[2] = frame.ballRadius;
    return packed;
}
