#include "render/gpu/probe_cascades.h"

#include <algorithm>
#include <cmath>

namespace {
    // value mod count, never negative.
    int wrapped(int value, int count) {
        const int remainder = value % count;
        return remainder < 0 ? remainder + count : remainder;
    }

    bool sameCoord(const ProbeGridCoord& a, const ProbeGridCoord& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    // At least 2 probes along each axis, the up axis giving way until they fit in one dispatch.
    ProbeCounts fittedCounts(int across, int high) {
        ProbeCounts counts{std::max(across, 2), std::max(high, 2), std::max(across, 2)};

        while (counts.y > 2 && probeCount(counts) > maxProbesPerVolume) {
            --counts.y;
        }

        return counts;
    }

    void addScaled(ProbeLight& sum, const ProbeLight& light, float weight) {
        sum.diffuse = sum.diffuse + light.diffuse * weight;
        sum.specular = sum.specular + light.specular * weight;
    }
}

/*
* Every probe has its own workgroup in one dispatch, so all the cascades together must fit in maxProbesPerVolume: the
* densest first (it matters most), then as many coarser ones as fit beside it. The atlases hold any number that fits.
*/
ProbeCascadeLayout probeCascadeLayout(const GlobalIlluminationSettings& settings) {
    ProbeCascadeLayout layout;
    layout.densest = fittedCounts(settings.densestProbesAcross, settings.densestProbesHigh);
    layout.counts = fittedCounts(settings.cascadeProbesAcross, settings.cascadeProbesHigh);
    layout.spacing = std::max(settings.probeSpacing, 0.01f);
    layout.cascades = std::clamp(settings.probeCascades, 1, maxProbeCascades);

    while (layout.cascades > 1 && probeCascadeLayoutProbes(layout) > maxProbesPerVolume) {
        --layout.cascades;
    }

    return layout;
}

ProbeCounts probeCascadeCounts(const ProbeCascadeLayout& layout, int cascade) {
    return cascade == 0 ? layout.densest : layout.counts;
}

int probeCascadeLayoutProbes(const ProbeCascadeLayout& layout) {
    return probeCount(layout.densest) + (layout.cascades - 1) * probeCount(layout.counts);
}

float probeCascadeSpacing(const ProbeCascadeLayout& layout, int cascade) {
    return std::ldexp(layout.spacing, cascade);
}

float probeLatticeOffset(const ProbeCascadeLayout& layout) {
    return probeLatticeOffsetShare * layout.spacing;
}

int probeWindowStart(float centre, float spacing, int count) {
    return static_cast<int>(std::floor(centre / spacing + 0.5f)) - (count - 1) / 2;
}

float probeCascadeReach(int count, float spacing, float biasReach) {
    return (static_cast<float>((count - 1) / 2) - 0.5f - probeCascadeEdgeCells) * spacing - biasReach;
}

ProbeCascade probeWindow(const Vec3& centre, float spacing, const ProbeCounts& counts, float selfShadowBias, int firstProbe, float latticeOffset) {
    const Vec3 spacings{spacing, spacing, spacing};
    const Vec3 offset{latticeOffset, latticeOffset, latticeOffset};
    const Vec3 fromLattice = centre - offset;
    const float biasReach = probeBiasReach(spacings, selfShadowBias);

    ProbeCascade cascade;
    cascade.start = ProbeGridCoord{probeWindowStart(fromLattice.x, spacing, counts.x), probeWindowStart(fromLattice.y, spacing, counts.y),
                                   probeWindowStart(fromLattice.z, spacing, counts.z)};
    cascade.volume.origin =
        offset + Vec3{static_cast<float>(cascade.start.x), static_cast<float>(cascade.start.y), static_cast<float>(cascade.start.z)} * spacing;
    cascade.volume.spacing = spacings;
    cascade.volume.counts = counts;
    cascade.volume.scroll = ProbeScroll{wrapped(cascade.start.x, counts.x), wrapped(cascade.start.y, counts.y), wrapped(cascade.start.z, counts.z)};
    cascade.firstProbe = firstProbe;
    cascade.weightCentre = centre;
    cascade.weightReach = Vec3{probeCascadeReach(counts.x, spacing, biasReach), probeCascadeReach(counts.y, spacing, biasReach),
                               probeCascadeReach(counts.z, spacing, biasReach)};
    cascade.weightFade = spacings * probeCascadeFadeCells;
    return cascade;
}

std::vector<ProbeCascade> probeCascadesAround(const Vec3& centre, const ProbeCascadeLayout& layout, float selfShadowBias) {
    std::vector<ProbeCascade> cascades;
    cascades.reserve(static_cast<std::size_t>(layout.cascades));

    int firstProbe = 0;

    for (int c = 0; c < layout.cascades; ++c) {
        const ProbeCounts counts = probeCascadeCounts(layout, c);
        cascades.push_back(probeWindow(centre, probeCascadeSpacing(layout, c), counts, selfShadowBias, firstProbe, probeLatticeOffset(layout)));
        firstProbe += probeCount(counts);
    }

    return cascades;
}

int probeCascadeHolding(std::span<const ProbeCascade> cascades, int probe) {
    int holding = 0;

    for (std::size_t c = 1; c < cascades.size(); ++c) {
        if (cascades[c].firstProbe <= probe) {
            holding = static_cast<int>(c);
        }
    }

    return holding;
}

int countCascadeProbes(std::span<const ProbeCascade> cascades) {
    int probes = 0;

    for (const ProbeCascade& cascade : cascades) {
        probes += probeCount(cascade.volume.counts);
    }

    return probes;
}

ProbeCascade fixedProbeCascade(const ProbeVolume& volume, int firstProbe) {
    const Vec3 halfSpan{volume.spacing.x * 0.5f * static_cast<float>(volume.counts.x - 1), volume.spacing.y * 0.5f * static_cast<float>(volume.counts.y - 1),
                        volume.spacing.z * 0.5f * static_cast<float>(volume.counts.z - 1)};
    ProbeCascade cascade;
    cascade.volume = volume;
    cascade.firstProbe = firstProbe;
    cascade.weightCentre = volume.origin + halfSpan;
    cascade.weightReach = halfSpan + volume.spacing * (probeVolumeMarginCells + probeVolumeFadeCells);
    cascade.weightFade = volume.spacing * probeVolumeFadeCells;
    return cascade;
}

float probeCascadeWeight(const ProbeCascade& cascade, const Vec3& point) {
    return probeCascadeWeight(cascade.weightCentre, cascade.weightReach, cascade.weightFade, point);
}

ProbeGridCoord probeLatticeCoord(const ProbeCascade& cascade, int slot) {
    const ProbeGridCoord grid = probeGridCoord(cascade.volume, slot);
    return ProbeGridCoord{cascade.start.x + grid.x, cascade.start.y + grid.y, cascade.start.z + grid.z};
}

std::vector<int> scrolledProbes(const ProbeCascade& before, const ProbeCascade& after) {
    const ProbeCounts& counts = after.volume.counts;
    const int probes = probeCount(counts);
    const bool resized = before.volume.counts.x != counts.x || before.volume.counts.y != counts.y || before.volume.counts.z != counts.z ||
                         before.volume.spacing.x != after.volume.spacing.x || before.volume.spacing.y != after.volume.spacing.y ||
                         before.volume.spacing.z != after.volume.spacing.z;
    std::vector<int> scrolled;

    for (int slot = 0; slot < probes; ++slot) {
        if (resized || !sameCoord(probeLatticeCoord(before, slot), probeLatticeCoord(after, slot))) {
            scrolled.push_back(after.firstProbe + slot);
        }
    }

    return scrolled;
}

ProbeLights sampleProbeCascades(std::span<const ProbeCascade> cascades, int fromCascade, float selfShadowBias, const Vec3& point, const Vec3& normal,
                                const Vec3& lookupNormal, const Vec3& reflected, const Vec3& toViewer, const ProbeLookup& lookup) {
    ProbeLights sum;
    float total = 0.0f;

    for (std::size_t c = static_cast<std::size_t>(std::max(fromCascade, 0)); c < cascades.size() && total < 1.0f; ++c) {
        const ProbeCascade& cascade = cascades[c];
        const float weight = probeCascadeWeight(cascade, point) * (1.0f - total);

        if (weight <= 0.0f) {
            continue;
        }

        const std::optional<ProbeLights> light =
            sampleTracedProbes(cascade.volume, selfShadowBias, point, normal, lookupNormal, reflected, toViewer, lookup, cascade.firstProbe);

        if (!light) {
            continue;
        }

        addScaled(sum.still, light->still, weight);
        addScaled(sum.fast, light->fast, weight);
        total += weight;
    }

    addScaled(sum.still, ProbeLight{lookup.ambient, lookup.ambient}, 1.0f - total);
    return sum;
}

std::vector<GpuProbeCascade> packProbeCascades(std::span<const ProbeCascade> cascades, const Vec3& cameraOrigin) {
    std::vector<GpuProbeCascade> packed;
    packed.reserve(cascades.size());

    for (const ProbeCascade& cascade : cascades) {
        const Vec3 origin = cascade.volume.origin - cameraOrigin;
        const Vec3 centre = cascade.weightCentre - cameraOrigin;
        GpuProbeCascade gpu{};
        gpu.origin[0] = origin.x;
        gpu.origin[1] = origin.y;
        gpu.origin[2] = origin.z;
        gpu.origin[3] = probeMaxDistance(cascade.volume.spacing);
        gpu.spacing[0] = cascade.volume.spacing.x;
        gpu.spacing[1] = cascade.volume.spacing.y;
        gpu.spacing[2] = cascade.volume.spacing.z;
        gpu.scroll[0] = cascade.volume.scroll.x;
        gpu.scroll[1] = cascade.volume.scroll.y;
        gpu.scroll[2] = cascade.volume.scroll.z;
        gpu.scroll[3] = cascade.firstProbe;
        gpu.weightCentre[0] = centre.x;
        gpu.weightCentre[1] = centre.y;
        gpu.weightCentre[2] = centre.z;
        gpu.weightReach[0] = cascade.weightReach.x;
        gpu.weightReach[1] = cascade.weightReach.y;
        gpu.weightReach[2] = cascade.weightReach.z;
        gpu.weightFade[0] = cascade.weightFade.x;
        gpu.weightFade[1] = cascade.weightFade.y;
        gpu.weightFade[2] = cascade.weightFade.z;
        gpu.counts[0] = cascade.volume.counts.x;
        gpu.counts[1] = cascade.volume.counts.y;
        gpu.counts[2] = cascade.volume.counts.z;
        packed.push_back(gpu);
    }

    return packed;
}

std::vector<GpuProbeState> buildCascadeStates(std::span<const ProbeCascade> cascades, std::span<const GpuProbeRelocation> relocations,
                                              std::span<const BoundingSphere> movers, const GlobalIlluminationSettings& settings) {
    std::vector<GpuProbeState> states;

    for (const ProbeCascade& cascade : cascades) {
        const std::size_t probes = static_cast<std::size_t>(probeCount(cascade.volume.counts));
        const std::size_t first = static_cast<std::size_t>(cascade.firstProbe);
        const std::span<const GpuProbeRelocation> own = first + probes <= relocations.size() ? relocations.subspan(first, probes)
                                                                                              : std::span<const GpuProbeRelocation>{};
        const std::vector<GpuProbeState> cascadeStates = buildProbeStates(cascade.volume, own, movers, settings);
        states.insert(states.end(), cascadeStates.begin(), cascadeStates.end());
    }

    return states;
}

std::vector<ProbeRelocationRequest> relocationRequests(std::span<const ProbeCascade> cascades, std::span<const int> probes) {
    std::vector<ProbeRelocationRequest> requests;
    requests.reserve(probes.size());

    for (const int probe : probes) {
        const ProbeCascade& cascade = cascades[static_cast<std::size_t>(probeCascadeHolding(cascades, probe))];
        requests.push_back(ProbeRelocationRequest{probe, probeLatticeCoord(cascade, probe - cascade.firstProbe)});
    }

    return requests;
}

GpuProbeRelocation pendingProbeRelocation() {
    GpuProbeRelocation pending;
    pending.off = -1.0f;
    return pending;
}

bool probeRelocationPending(const GpuProbeRelocation& relocation) {
    return relocation.off < 0.0f;
}

void keepProbeRelocations(std::vector<GpuProbeRelocation>& records, std::span<const ProbeRelocationRequest> requests,
                          std::span<const GpuProbeRelocation> found, std::span<const ProbeCascade> cascades) {
    for (std::size_t i = 0; i < requests.size() && i < found.size(); ++i) {
        const ProbeRelocationRequest& request = requests[i];
        const ProbeCascade& cascade = cascades[static_cast<std::size_t>(probeCascadeHolding(cascades, request.probe))];

        if (static_cast<std::size_t>(request.probe) < records.size() &&
            sameCoord(probeLatticeCoord(cascade, request.probe - cascade.firstProbe), request.place)) {
            records[static_cast<std::size_t>(request.probe)] = found[i];
        }
    }
}

std::vector<ProbeSeed> chooseProbeSeeds(std::span<const int> restarted, std::span<const ProbeCascade> cascades) {
    std::vector<ProbeSeed> seeds;

    if (cascades.size() < 2) {
        return seeds;
    }

    const int coarsest = cascades.back().firstProbe;

    for (const int probe : restarted) {
        if (probe < coarsest && static_cast<int>(seeds.size()) < maxProbeSeeds) {
            seeds.push_back(ProbeSeed{probe, static_cast<int>(seeds.size())});
        }
    }

    return seeds;
}
