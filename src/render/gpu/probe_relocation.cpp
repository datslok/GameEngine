#include "render/gpu/probe_relocation.h"
#include "render/gpu/ddgi_sampling.h"

#include <algorithm>
#include <cmath>

bool probeTraced(ProbeState state) {
    return state == ProbeState::Awake || state == ProbeState::Vigilant;
}

void tallyProbeRay(ProbeRayTally& tally, const Vec3& direction, float signedDistance, const Vec3& start, const Vec3& spacing) {
    ++tally.rays;

    if (signedDistance >= probeRayMiss) {
        return;
    }

    if (signedDistance < 0.0f) {
        ++tally.backFaces;

        if (-signedDistance < tally.closestBackFace) {
            tally.closestBackFace = -signedDistance;
            tally.closestBackFaceDirection = direction;
        }

        return;
    }

    ++tally.frontFaces;

    if (signedDistance < tally.closestFrontFace) {
        tally.closestFrontFace = signedDistance;
        tally.closestFrontFaceDirection = direction;
    }

    if (signedDistance > tally.farthestFrontFace) {
        tally.farthestFrontFace = signedDistance;
        tally.farthestFrontFaceDirection = direction;
    }

    const Vec3 hit = start + direction * signedDistance;
    const float beyond = std::max({std::abs(hit.x) - spacing.x, std::abs(hit.y) - spacing.y, std::abs(hit.z) - spacing.z});
    tally.surfaceBeyondCells = std::min(tally.surfaceBeyondCells, beyond);
}

float probeEscapeScale(const Vec3& offset, const Vec3& step, const Vec3& limit) {
    const float offsets[3] = {offset.x, offset.y, offset.z};
    const float steps[3] = {step.x, step.y, step.z};
    const float limits[3] = {limit.x, limit.y, limit.z};
    float scale = std::numeric_limits<float>::max();

    for (int axis = 0; axis < 3; ++axis) {
        if (steps[axis] > 0.0f) {
            scale = std::min(scale, (limits[axis] - offsets[axis]) / steps[axis]);
        } else if (steps[axis] < 0.0f) {
            scale = std::min(scale, (-limits[axis] - offsets[axis]) / steps[axis]);
        }
    }

    const float length = step.length();
    return length > 0.0f ? scale - probeEscapeMargin / length : 0.0f;
}

namespace {
    bool insideSomething(const ProbeRayTally& tally) {
        return static_cast<float>(tally.backFaces) > insideBackFaceShare * static_cast<float>(tally.rays);
    }
}

ProbeMove relocateProbeStep(const ProbeRayTally& tally, const Vec3& offset, const Vec3& spacing) {
    const Vec3 limit = spacing * probeOffsetLimit;

    if (insideSomething(tally)) {
        const Vec3 step = tally.closestBackFaceDirection * tally.closestBackFace;
        const float scale = probeEscapeScale(offset, step, limit);

        if (scale <= 1.0f) {
            return ProbeMove{offset, true};
        }

        return ProbeMove{offset + step * scale, false};
    }

    const float smallestSpacing = std::min({spacing.x, spacing.y, spacing.z});

    if (tally.frontFaces > 0 && tally.closestFrontFace < probeMinFrontFaceShare * smallestSpacing &&
        tally.closestFrontFaceDirection.dot(tally.farthestFrontFaceDirection) <= sameSurfaceCosine) {
        const Vec3 moved = offset + tally.farthestFrontFaceDirection * std::min(tally.farthestFrontFace, probeFrontFaceNudge * smallestSpacing);
        return ProbeMove{Vec3{std::clamp(moved.x, -limit.x, limit.x), std::clamp(moved.y, -limit.y, limit.y), std::clamp(moved.z, -limit.z, limit.z)},
                         false};
    }

    return ProbeMove{offset, false};
}

GpuProbeRelocation finishProbeRelocation(const ProbeRayTally& tally, const Vec3& offset, bool stuck) {
    GpuProbeRelocation relocation;
    relocation.offset[0] = offset.x;
    relocation.offset[1] = offset.y;
    relocation.offset[2] = offset.z;
    relocation.off = stuck || insideSomething(tally) ? 1.0f : 0.0f;
    relocation.surfaceBeyondCells = tally.surfaceBeyondCells;
    return relocation;
}

GpuProbeRelocation relocateProbe(const Vec3& gridPosition, const Vec3& spacing, int iterations, const ProbeRayTrace& trace) {
    const auto tallyAt = [&](const Vec3& offset) {
        ProbeRayTally tally;

        for (int ray = 0; ray < probeRelocationRays; ++ray) {
            const Vec3 direction = sphericalFibonacci(ray, probeRelocationRays);
            tallyProbeRay(tally, direction, trace(gridPosition + offset, direction), offset, spacing);
        }

        return tally;
    };

    Vec3 offset{0.0f, 0.0f, 0.0f};
    bool stuck = false;

    for (int iteration = 0; iteration < iterations; ++iteration) {
        const ProbeMove move = relocateProbeStep(tallyAt(offset), offset, spacing);

        if (move.stuck) {
            stuck = true;
            break;
        }

        if (move.offset.x == offset.x && move.offset.y == offset.y && move.offset.z == offset.z) {
            break;
        }

        offset = move.offset;
    }

    return finishProbeRelocation(tallyAt(offset), offset, stuck);
}

ProbeState classifyProbe(const GpuProbeRelocation& relocation, float biasReach) {
    if (relocation.off != 0.0f) {
        return ProbeState::Off;
    }

    return relocation.surfaceBeyondCells <= biasReach ? ProbeState::Vigilant : ProbeState::Sleeping;
}

namespace {
    // The grid layers along one axis whose probes lie within reach of centre: those from ceil to floor of the range in
    // cells, clamped to the volume.
    struct LayerRange {
        int first = 0;
        int last = -1;
    };

    LayerRange layersWithin(float centre, float reach, float origin, float spacing, int count) {
        const float first = std::ceil((centre - reach - origin) / spacing);
        const float last = std::floor((centre + reach - origin) / spacing);
        return LayerRange{static_cast<int>(std::max(first, 0.0f)), static_cast<int>(std::min(last, static_cast<float>(count - 1)))};
    }
}

std::vector<GpuProbeState> buildProbeStates(const ProbeVolume& volume, std::span<const GpuProbeRelocation> relocations,
                                            std::span<const BoundingSphere> movers, const GlobalIlluminationSettings& settings) {
    const std::size_t probes = static_cast<std::size_t>(probeCount(volume.counts));
    std::vector<GpuProbeState> states(probes);

    if (relocations.size() != probes) {
        return states;
    }

    const float biasReach = probeBiasReach(volume.spacing, settings.selfShadowBias);

    for (std::size_t probe = 0; probe < probes; ++probe) {
        const GpuProbeRelocation& relocation = relocations[probe];

        if (settings.probeRelocation) {
            std::copy(std::begin(relocation.offset), std::end(relocation.offset), std::begin(states[probe].offset));
        }

        if (settings.probeStates) {
            states[probe].state = static_cast<std::int32_t>(classifyProbe(relocation, biasReach));
        }
    }

    if (!settings.probeStates) {
        return states;
    }

    const Vec3 cellsReach = volume.spacing * (1.0f + settings.wakeCells);

    for (const BoundingSphere& mover : movers) {
        if (mover.radius < 0.0f) {
            continue;
        }

        const Vec3 reach = cellsReach + Vec3{biasReach + mover.radius, biasReach + mover.radius, biasReach + mover.radius};
        const LayerRange xs = layersWithin(mover.centre.x, reach.x, volume.origin.x, volume.spacing.x, volume.counts.x);
        const LayerRange ys = layersWithin(mover.centre.y, reach.y, volume.origin.y, volume.spacing.y, volume.counts.y);
        const LayerRange zs = layersWithin(mover.centre.z, reach.z, volume.origin.z, volume.spacing.z, volume.counts.z);

        for (int z = zs.first; z <= zs.last; ++z) {
            for (int y = ys.first; y <= ys.last; ++y) {
                for (int x = xs.first; x <= xs.last; ++x) {
                    std::int32_t& state = states[static_cast<std::size_t>(probeSlot(volume, ProbeGridCoord{x, y, z}))].state;

                    if (state == static_cast<std::int32_t>(ProbeState::Sleeping)) {
                        state = static_cast<std::int32_t>(ProbeState::Awake);
                    }
                }
            }
        }
    }

    return states;
}

std::vector<BoundingSphere> movingBounds(std::span<const DrawItem> draws) {
    std::vector<BoundingSphere> movers;

    for (const DrawItem& draw : draws) {
        if (!draw.isStatic && draw.bounds.radius >= 0.0f) {
            movers.push_back(draw.bounds);
        }
    }

    return movers;
}

ProbeStateCounts countProbeStates(std::span<const GpuProbeState> states) {
    ProbeStateCounts counts;
    counts.probes = static_cast<int>(states.size());

    for (const GpuProbeState& probe : states) {
        const ProbeState state = static_cast<ProbeState>(probe.state);
        counts.traced += probeTraced(state) ? 1 : 0;
        counts.awake += state == ProbeState::Awake ? 1 : 0;
        counts.sleeping += state == ProbeState::Sleeping ? 1 : 0;
        counts.off += state == ProbeState::Off ? 1 : 0;
        counts.moved += probe.offset[0] != 0.0f || probe.offset[1] != 0.0f || probe.offset[2] != 0.0f ? 1 : 0;
    }

    return counts;
}

std::vector<int> restartedProbes(std::span<const GpuProbeState> before, std::span<const GpuProbeState> after) {
    std::vector<int> restarted;

    if (before.size() != after.size()) {
        return restarted;
    }

    for (std::size_t probe = 0; probe < after.size(); ++probe) {
        const bool tracedNow = probeTraced(static_cast<ProbeState>(after[probe].state));
        const bool tracedBefore = probeTraced(static_cast<ProbeState>(before[probe].state));
        const bool moved = !std::equal(std::begin(before[probe].offset), std::end(before[probe].offset), std::begin(after[probe].offset));

        if (tracedNow && (!tracedBefore || moved)) {
            restarted.push_back(static_cast<int>(probe));
        }
    }

    return restarted;
}
