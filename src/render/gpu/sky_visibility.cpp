#include "render/gpu/sky_visibility.h"

#include "core/half_float.h"
#include "render/gpu/ddgi.h"
#include "render/gpu/ddgi_sampling.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
    // Each ray of N even ones stands for 4 pi / N of the sphere: a = (1 / 4 pi) sum V (4 pi / N), b three times as much of V w.
    constexpr float leanScale = 3.0f;

    // The clamped cosine's second band over its first, (2 pi / 3) / pi: how much the lean counts on a surface.
    constexpr float facingLean = 2.0f / 3.0f;

    // sum += value times weight, both bands.
    void addWeighted(SkyVisibility& sum, const SkyVisibility& value, float weight) {
        sum.open += value.open * weight;
        sum.lean = sum.lean + value.lean * weight;
    }

    SkyVisibility scaled(const SkyVisibility& value, float factor) {
        return SkyVisibility{value.open * factor, value.lean * factor};
    }

    // The sky's share and each bounce channel's, each within 0..1, added and at least floor.
    Vec3 lightOf(float sky, float red, float green, float blue, float floor) {
        return Vec3{std::max(sky + red, floor), std::max(sky + green, floor), std::max(sky + blue, floor)};
    }

    std::uint32_t packHalves(float low, float high) {
        return static_cast<std::uint32_t>(floatToHalf(low)) | (static_cast<std::uint32_t>(floatToHalf(high)) << 16);
    }

    std::uint32_t reachByte(float reach) {
        return static_cast<std::uint32_t>(std::lround(std::clamp(reach, 0.0f, 1.0f) * static_cast<float>(skyVisibilityReachSteps)));
    }

    float reachOfByte(std::uint32_t word, int byte) {
        return static_cast<float>((word >> (8 * byte)) & 0xFFu) / static_cast<float>(skyVisibilityReachSteps);
    }

    // The weight of one of a cell's eight points, as the probes weigh theirs, without their distance test.
    float cornerWeight(const Vec3& normal, const Vec3& toCorner, float trilinear) {
        return crushProbeWeight(probeBackFaceWeight(normal, toCorner)) * trilinear;
    }

    // One cascade's blend at a point (known unset), or nothing when no point round it is baked, out of the walls and in sight.
    std::optional<SkySample> sampleCascade(const ProbeCascade& cascade, std::span<const SkyCell> cells, float lookupBias, const Vec3& point,
                                           const Vec3& normal, const Vec3& toViewer) {
        const ProbeVolume& volume = cascade.volume;
        const Vec3 biased = point + probeSelfShadowBias(normal, toViewer, volume.spacing, lookupBias);
        const ProbeCell cell = probeCell(volume, biased);
        float total = 0.0f;
        SkySample sum{SkyVisibility{0.0f, Vec3{0.0f, 0.0f, 0.0f}}, noSkyBounce, 0.0f};

        for (int corner = 0; corner < 8; ++corner) {
            const int slot = probeSlot(volume, probeCellCorner(cell, volume.counts, corner));
            const SkyCell& sky = cells[static_cast<std::size_t>(cascade.firstProbe + slot)];

            if (sky.state != SkyCellState::Baked) {
                continue;
            }

            const Vec3 position = probePosition(volume, slot);

            if (!skyCornerSees(sky.reach, biased - position, volume.spacing.x)) {
                continue;
            }

            const float weight = cornerWeight(normal, position - point, probeTrilinearWeight(cell.alpha, corner));
            addWeighted(sum.visibility, sky.visibility, weight);

            for (std::size_t channel = 0; channel < sum.bounce.size(); ++channel) {
                addWeighted(sum.bounce[channel], sky.bounce[channel], weight);
            }

            total += weight;
        }

        if (total <= 0.0f) {
            return std::nullopt;
        }

        const float share = 1.0f / total;
        sum.visibility = scaled(sum.visibility, share);

        for (SkyVisibility& channel : sum.bounce) {
            channel = scaled(channel, share);
        }

        return sum;
    }
}

void visit(SkyVisibilitySettings& settings, SettingsVisitor& visitor) {
    visitor.field("Enabled", settings.enabled);
    visitor.field("Ambient floor", settings.ambientFloor, {.min = 0.0f, .max = 1.0f, .sliderMin = 0.0f, .sliderMax = 0.2f});
    visitor.field("Spacing", settings.spacing, {.min = 0.05f, .max = 16.0f, .sliderMin = 0.25f, .sliderMax = 2.0f, .unit = "m"});
    visitor.field("Cascades", settings.cascades, {.min = 1, .max = maxSkyVisibilityCascades});
    visitor.field("Rays", settings.rays, {.min = 1, .max = 1024, .sliderMin = 8, .sliderMax = 256});
    visitor.field("Quick rays", settings.quickRays, {.min = 1, .max = 1024, .sliderMin = 8, .sliderMax = 256});
    visitor.field("Store megabytes", settings.storeMegabytes, {.min = 0.0f, .max = 16384.0f, .sliderMin = 0.0f, .sliderMax = 1024.0f, .unit = "MB"});
    visitor.field("Ray length", settings.rayLength, {.min = 0.1f, .max = 1000.0f, .sliderMin = 5.0f, .sliderMax = 100.0f, .unit = "m"});
    visitor.field("Lookup bias", settings.lookupBias, {.min = 0.0f, .max = 4.0f, .sliderMin = 0.0f, .sliderMax = 1.0f});
    visitor.field("Bake milliseconds", settings.bakeMilliseconds, {.min = 0.0f, .max = 100.0f, .sliderMin = 0.0f, .sliderMax = 8.0f, .unit = "ms"});
    visitor.field("Debug view", settings.debugView);
}

// Written so not a number fails too.
void validateSkyVisibility(const SkyVisibilitySettings& settings) {
    if (!(settings.ambientFloor >= 0.0f && settings.ambientFloor <= 1.0f)) {
        throw std::invalid_argument("Sky visibility needs an ambient floor from 0 to 1");
    }

    if (!(settings.spacing > 0.0f) || !(settings.rayLength > 0.0f)) {
        throw std::invalid_argument("Sky visibility needs a spacing and a ray length above 0");
    }

    if (!(settings.bakeMilliseconds >= 0.0f) || !(settings.lookupBias >= 0.0f) || !(settings.storeMegabytes >= 0.0f)) {
        throw std::invalid_argument("Sky visibility needs a bake time, a lookup bias and a store of at least 0");
    }

    if (settings.cascades < 1 || settings.cascades > maxSkyVisibilityCascades || settings.rays < 1 || settings.quickRays < 1) {
        throw std::invalid_argument("Sky visibility needs 1 to SKY_VISIBILITY_MAX_CASCADES cascades and at least 1 ray and quick ray");
    }
}

bool sameSkyVisibilityGrid(const SkyVisibilitySettings& a, const SkyVisibilitySettings& b) {
    return a.spacing == b.spacing && a.cascades == b.cascades && a.rays == b.rays && a.rayLength == b.rayLength;
}

Vec3 skyVisibilityRay(int ray, int rays) {
    return sphericalFibonacci(ray, rays);
}

SkyVisibility skyVisibilityFromRays(std::span<const Vec3> directions, std::span<const float> openings) {
    const std::size_t count = std::min(directions.size(), openings.size());

    if (count == 0) {
        return SkyVisibility{};
    }

    const float share = 1.0f / static_cast<float>(count);
    Vec3 meanDirection{0.0f, 0.0f, 0.0f};

    for (std::size_t ray = 0; ray < count; ++ray) {
        meanDirection = meanDirection + directions[ray];
    }

    meanDirection = meanDirection * share;
    float open = 0.0f;
    Vec3 lean{0.0f, 0.0f, 0.0f};

    for (std::size_t ray = 0; ray < count; ++ray) {
        open += openings[ray];
        lean = lean + (directions[ray] - meanDirection) * openings[ray];
    }

    return SkyVisibility{open * share, lean * (leanScale * share)};
}

float skySeenFacing(const SkyVisibility& visibility, const Vec3& normal) {
    return std::clamp(visibility.open + facingLean * visibility.lean.dot(normal), 0.0f, 1.0f);
}

float skySeenAlong(const SkyVisibility& visibility, const Vec3& direction) {
    return std::clamp(visibility.open + visibility.lean.dot(direction), 0.0f, 1.0f);
}

Vec3 skyLightFacing(const SkyVisibility& visibility, const SkyBounce& bounce, const Vec3& normal, float floor) {
    return lightOf(skySeenFacing(visibility, normal), skySeenFacing(bounce[0], normal), skySeenFacing(bounce[1], normal), skySeenFacing(bounce[2], normal), floor);
}

Vec3 skyLightAlong(const SkyVisibility& visibility, const SkyBounce& bounce, const Vec3& direction, float floor) {
    return lightOf(skySeenAlong(visibility, direction), skySeenAlong(bounce[0], direction), skySeenAlong(bounce[1], direction), skySeenAlong(bounce[2], direction),
                   floor);
}

Vec3 skyLightInAir(const SkyVisibility& visibility, const SkyBounce& bounce, float floor) {
    const auto mean = [](const SkyVisibility& bands) { return std::clamp(bands.open, 0.0f, 1.0f); };
    return lightOf(mean(visibility), mean(bounce[0]), mean(bounce[1]), mean(bounce[2]), floor);
}

Vec3 skyReachDirection(int direction) {
    const float sign = (direction & 1) != 0 ? -1.0f : 1.0f;

    switch (direction / 2) {
        case 0:
            return Vec3{sign, 0.0f, 0.0f};
        case 1:
            return Vec3{0.0f, sign, 0.0f};
        default:
            return Vec3{0.0f, 0.0f, sign};
    }
}

bool skyCornerSees(const std::array<float, skyReachDirections>& reach, const Vec3& fromCorner, float spacing) {
    const float slack = 1.0f / static_cast<float>(skyVisibilityReachSteps);
    const float offsets[3] = {fromCorner.x / spacing, fromCorner.y / spacing, fromCorner.z / spacing};

    for (int axis = 0; axis < 3; ++axis) {
        const float along = offsets[axis];
        const float allowed = reach[static_cast<std::size_t>(2 * axis + (along < 0.0f ? 1 : 0))];

        if (std::abs(along) > allowed + slack) {
            return false;
        }
    }

    return true;
}

GpuSkyCell packSkyCell(const SkyCell& cell) {
    GpuSkyCell packed{};
    packed.words[0] = packHalves(cell.visibility.open, cell.visibility.lean.x);
    packed.words[1] = packHalves(cell.visibility.lean.y, cell.visibility.lean.z);
    packed.words[2] = reachByte(cell.reach[0]) | (reachByte(cell.reach[1]) << 8) | (reachByte(cell.reach[2]) << 16) | (reachByte(cell.reach[3]) << 24);
    packed.words[3] = reachByte(cell.reach[4]) | (reachByte(cell.reach[5]) << 8) | (static_cast<std::uint32_t>(cell.state) << 24);

    for (std::size_t channel = 0; channel < cell.bounce.size(); ++channel) {
        const SkyVisibility& bands = cell.bounce[channel];
        packed.words[4 + 2 * channel] = packHalves(bands.open, bands.lean.x);
        packed.words[5 + 2 * channel] = packHalves(bands.lean.y, bands.lean.z);
    }

    return packed;
}

SkyCell unpackSkyCell(const GpuSkyCell& packed) {
    SkyCell cell;
    cell.visibility.open = halfToFloat(static_cast<std::uint16_t>(packed.words[0] & 0xFFFFu));
    cell.visibility.lean = Vec3{halfToFloat(static_cast<std::uint16_t>(packed.words[0] >> 16)), halfToFloat(static_cast<std::uint16_t>(packed.words[1] & 0xFFFFu)),
                                halfToFloat(static_cast<std::uint16_t>(packed.words[1] >> 16))};

    for (int direction = 0; direction < 4; ++direction) {
        cell.reach[static_cast<std::size_t>(direction)] = reachOfByte(packed.words[2], direction);
    }

    cell.reach[4] = reachOfByte(packed.words[3], 0);
    cell.reach[5] = reachOfByte(packed.words[3], 1);
    cell.state = static_cast<SkyCellState>(packed.words[3] >> 24);

    for (std::size_t channel = 0; channel < cell.bounce.size(); ++channel) {
        const std::uint32_t first = packed.words[4 + 2 * channel];
        const std::uint32_t second = packed.words[5 + 2 * channel];
        cell.bounce[channel] = SkyVisibility{halfToFloat(static_cast<std::uint16_t>(first & 0xFFFFu)),
                                             Vec3{halfToFloat(static_cast<std::uint16_t>(first >> 16)), halfToFloat(static_cast<std::uint16_t>(second & 0xFFFFu)),
                                                  halfToFloat(static_cast<std::uint16_t>(second >> 16))}};
    }

    return cell;
}

float skyVisibilitySpacing(const SkyVisibilitySettings& settings, int cascade) {
    return std::ldexp(settings.spacing, cascade);
}

std::vector<ProbeCascade> skyVisibilityCascades(const Vec3& centre, const SkyVisibilitySettings& settings) {
    const int count = std::clamp(settings.cascades, 1, maxSkyVisibilityCascades);
    const int perCascade = probeCount(skyVisibilityCounts);
    const float latticeOffset = probeLatticeOffsetShare * settings.spacing;
    std::vector<ProbeCascade> cascades;
    cascades.reserve(static_cast<std::size_t>(count));

    for (int c = 0; c < count; ++c) {
        cascades.push_back(probeWindow(centre, skyVisibilitySpacing(settings, c), skyVisibilityCounts, settings.lookupBias, c * perCascade, latticeOffset));
    }

    return cascades;
}

int skyVisibilityCellCount(const SkyVisibilitySettings& settings) {
    return std::clamp(settings.cascades, 1, maxSkyVisibilityCascades) * probeCount(skyVisibilityCounts);
}

SkySample sampleSkyVisibility(std::span<const ProbeCascade> cascades, std::span<const SkyCell> cells, float lookupBias, const Vec3& point,
                              const Vec3& normal, const Vec3& toViewer) {
    SkySample sum{SkyVisibility{0.0f, Vec3{0.0f, 0.0f, 0.0f}}, noSkyBounce, 0.0f};
    float total = 0.0f;

    for (const ProbeCascade& cascade : cascades) {
        if (total >= 1.0f) {
            break;
        }

        const float weight = probeCascadeWeight(cascade, point) * (1.0f - total);

        if (weight <= 0.0f) {
            continue;
        }

        const std::optional<SkySample> read = sampleCascade(cascade, cells, lookupBias, point, normal, toViewer);

        if (!read) {
            sum.missing += weight;
            continue;
        }

        addWeighted(sum.visibility, read->visibility, weight);

        for (std::size_t channel = 0; channel < sum.bounce.size(); ++channel) {
            addWeighted(sum.bounce[channel], read->bounce[channel], weight);
        }

        total += weight;
    }

    // What the cascades leave is an open field (skyOpenField), with nothing bounced.
    sum.visibility.open += skyOpenField.open * (1.0f - total);
    sum.visibility.lean = sum.visibility.lean + skyOpenField.lean * (1.0f - total);
    sum.known = total;

    // A denser cascade's gap is offered to the next one too, so it may be counted twice: never more than is unknown.
    sum.missing = std::min(sum.missing, 1.0f - total);
    return sum;
}

SkySample skyMissingLeftDark(const SkySample& sample) {
    SkySample dark = sample;
    dark.visibility.open -= skyOpenField.open * sample.missing;
    dark.visibility.lean = dark.visibility.lean - skyOpenField.lean * sample.missing;
    return dark;
}

GpuSkyVisibilityOptions packSkyVisibilityOptions(const SkyVisibilitySettings& settings, int cascadesRead) {
    if (cascadesRead <= 0) {
        return GpuSkyVisibilityOptions{{0.0f, 0.0f, 0.0f, 0.0f}};
    }

    return GpuSkyVisibilityOptions{{static_cast<float>(cascadesRead), settings.ambientFloor, settings.lookupBias, settings.debugView ? 1.0f : 0.0f}};
}
