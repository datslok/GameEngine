#include "render/gpu/ddgi_sampling.h"
#include "math/aabb.h"
#include "render/gpu/clusters.h"
#include "render/gpu/light_falloff.h"

#include <algorithm>
#include <cmath>

namespace {
    float component(const Vec3& value, int axis) {
        return axis == 0 ? value.x : axis == 1 ? value.y : value.z;
    }

    int count(const ProbeCounts& counts, int axis) {
        return axis == 0 ? counts.x : axis == 1 ? counts.y : counts.z;
    }

    // The point in cells from the first probe along each axis.
    Vec3 cellsFromOrigin(const ProbeVolume& volume, const Vec3& point) {
        const Vec3 offset = point - volume.origin;
        return Vec3{offset.x / volume.spacing.x, offset.y / volume.spacing.y, offset.z / volume.spacing.z};
    }

    // The irradiance on a surface from a light along offsetToLight giving this radiance there (already with falloff).
    Vec3 facingIrradiance(const Vec3& radiance, const Vec3& toLight, const Vec3& normal) {
        return radiance * std::max(0.0f, normal.dot(toLight));
    }
}

Vec3 probeSelfShadowBias(const Vec3& normal, const Vec3& toViewer, const Vec3& spacing, float selfShadowBias) {
    const float smallestSpacing = std::min({spacing.x, spacing.y, spacing.z});
    return (normal * biasNormalShare + toViewer * biasViewShare) * (biasSpacingShare * smallestSpacing * selfShadowBias);
}

float probeBiasReach(const Vec3& spacing, float selfShadowBias) {
    const float smallestSpacing = std::min({spacing.x, spacing.y, spacing.z});
    return (biasNormalShare + biasViewShare) * biasSpacingShare * smallestSpacing * selfShadowBias;
}

ProbeCell probeCell(const ProbeVolume& volume, const Vec3& point) {
    const Vec3 cells = cellsFromOrigin(volume, point);
    int base[3];
    float alpha[3];

    for (int axis = 0; axis < 3; ++axis) {
        const float along = component(cells, axis);
        base[axis] = std::clamp(static_cast<int>(std::floor(along)), 0, count(volume.counts, axis) - 1);
        alpha[axis] = std::clamp(along - static_cast<float>(base[axis]), 0.0f, 1.0f);
    }

    return ProbeCell{ProbeGridCoord{base[0], base[1], base[2]}, Vec3{alpha[0], alpha[1], alpha[2]}};
}

ProbeGridCoord probeCellCorner(const ProbeCell& cell, const ProbeCounts& counts, int corner) {
    return ProbeGridCoord{std::min(cell.base.x + (corner & 1), counts.x - 1), std::min(cell.base.y + ((corner >> 1) & 1), counts.y - 1),
                          std::min(cell.base.z + ((corner >> 2) & 1), counts.z - 1)};
}

float probeTrilinearWeight(const Vec3& alpha, int corner) {
    float weight = 1.0f;

    for (int axis = 0; axis < 3; ++axis) {
        const float share = component(alpha, axis);
        const float factor = ((corner >> axis) & 1) != 0 ? share : 1.0f - share;
        weight *= std::max(factor, minTrilinearFactor);
    }

    return weight;
}

float probeBackFaceWeight(const Vec3& normal, const Vec3& toProbe) {
    const float length = toProbe.length();
    const float facing = length > 0.0f ? normal.dot(toProbe) / length : 1.0f;
    const float wrapped = (facing + 1.0f) * 0.5f;
    return wrapped * wrapped + probeWrapFloor;
}

float probeVisibility(float distance, const Vec2& moments) {
    const float mean = moments.x;

    if (distance <= mean) {
        return 1.0f;
    }

    const float variance = std::abs(moments.y - mean * mean);
    const float beyond = distance - mean;
    const float bound = variance / (variance + beyond * beyond);
    return std::max(bound * bound * bound, minProbeVisibility);
}

float crushProbeWeight(float weight) {
    const float kept = std::max(weight, smallestProbeWeight);

    if (kept < probeCrushThreshold) {
        return kept * kept * kept / (probeCrushThreshold * probeCrushThreshold);
    }

    return kept;
}

float probeCornerWeight(const Vec3& normal, const Vec3& toProbe, float distance, const Vec2& moments, float trilinear) {
    return crushProbeWeight(probeBackFaceWeight(normal, toProbe) * probeVisibility(distance, moments)) * trilinear;
}

float probeCascadeWeight(const Vec3& centre, const Vec3& reach, const Vec3& fade, const Vec3& point) {
    float weight = 1.0f;

    for (int axis = 0; axis < 3; ++axis) {
        const float distance = std::abs(component(point, axis) - component(centre, axis));
        weight *= std::clamp((component(reach, axis) - distance) / component(fade, axis), 0.0f, 1.0f);
    }

    return weight;
}

// Never below zero first, so a value a filter rounded a hair negative has a square root.
void addProbeSample(ProbeBlend& blend, const Vec3& irradiance, float weight) {
    const Vec3 root{std::sqrt(std::max(irradiance.x, 0.0f)), std::sqrt(std::max(irradiance.y, 0.0f)), std::sqrt(std::max(irradiance.z, 0.0f))};
    blend.sum = blend.sum + root * weight;
    blend.weight += weight;
}

Vec3 finishProbeBlend(const ProbeBlend& blend) {
    const Vec3 root = blend.sum * (1.0f / blend.weight);
    return root * root;
}

/*
* Visibility is tested from each probe towards the biased point; which side of the surface a probe lies on is judged
* from the surface point itself. All four blends (each atlas, diffuse and specular) share each corner's weight. Without
* a fast lookup the fast part reads nothing.
*/
ProbeLights sampleProbes(const ProbeVolume& volume, float selfShadowBias, const Vec3& point, const Vec3& normal, const Vec3& lookupNormal,
                         const Vec3& reflected, const Vec3& toViewer, const ProbeLookup& lookup, int firstProbe) {
    return sampleTracedProbes(volume, selfShadowBias, point, normal, lookupNormal, reflected, toViewer, lookup, firstProbe)
        .value_or(ProbeLights{ProbeLight{lookup.ambient, lookup.ambient}, ProbeLight{}});
}

std::optional<ProbeLights> sampleTracedProbes(const ProbeVolume& volume, float selfShadowBias, const Vec3& point, const Vec3& normal,
                                              const Vec3& lookupNormal, const Vec3& reflected, const Vec3& toViewer, const ProbeLookup& lookup,
                                              int firstProbe) {
    const Vec3 biased = point + probeSelfShadowBias(normal, toViewer, volume.spacing, selfShadowBias);
    const ProbeCell cell = probeCell(volume, biased);
    const Vec3 none{0.0f, 0.0f, 0.0f};
    ProbeBlend diffuse;
    ProbeBlend specular;
    ProbeBlend fastDiffuse;
    ProbeBlend fastSpecular;

    for (int corner = 0; corner < 8; ++corner) {
        const ProbeGridCoord grid = probeCellCorner(cell, volume.counts, corner);
        const int slot = probeSlot(volume, grid);
        const int index = firstProbe + slot;
        const GpuProbeState state = lookup.state ? lookup.state(index) : GpuProbeState{};

        if (!probeTraced(static_cast<ProbeState>(state.state))) {
            continue;
        }

        const Vec3 probe = probePosition(volume, slot) + Vec3{state.offset[0], state.offset[1], state.offset[2]};
        const Vec3 fromProbe = biased - probe;
        const float distance = fromProbe.length();
        const Vec3 direction = distance > 0.0f ? fromProbe * (1.0f / distance) : normal;

        const float weight = probeCornerWeight(normal, probe - point, distance, lookup.moments(index, direction),
                                               probeTrilinearWeight(cell.alpha, corner));
        addProbeSample(diffuse, lookup.irradiance(index, lookupNormal), weight);
        addProbeSample(specular, lookup.irradiance(index, reflected), weight);
        addProbeSample(fastDiffuse, lookup.fastIrradiance ? lookup.fastIrradiance(index, lookupNormal) : none, weight);
        addProbeSample(fastSpecular, lookup.fastIrradiance ? lookup.fastIrradiance(index, reflected) : none, weight);
    }

    if (diffuse.weight <= 0.0f) {
        return std::nullopt;
    }

    return ProbeLights{ProbeLight{finishProbeBlend(diffuse), finishProbeBlend(specular)},
                       ProbeLight{finishProbeBlend(fastDiffuse), finishProbeBlend(fastSpecular)}};
}

Vec3 pointLightAtHit(const Vec3& offsetToLight, float range, const Vec3& radiance, float sourceRadius, const Vec3& normal) {
    const float distance = offsetToLight.length();

    if (distance <= 0.0f || distance >= range) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    const Vec3 toLight = offsetToLight * (1.0f / distance);
    return facingIrradiance(radiance * distanceFalloff(distance, range, sourceRadius), toLight, normal);
}

Vec3 spotLightAtHit(const Vec3& offsetToLight, float range, const Vec3& radiance, float sourceRadius, const Vec3& beamDirection, float cosOuter,
                    float cosInner, const Vec3& normal) {
    const float distance = offsetToLight.length();

    if (distance <= 0.0f || distance >= range) {
        return Vec3{0.0f, 0.0f, 0.0f};
    }

    const Vec3 toLight = offsetToLight * (1.0f / distance);
    const float cone = spotCone((toLight * -1.0f).dot(beamDirection), cosOuter, cosInner);
    return facingIrradiance(radiance * (distanceFalloff(distance, range, sourceRadius) * cone), toLight, normal);
}

float hitLightStrength(const Vec3& irradiance) {
    return probeLuminance(irradiance);
}

/*
* Kept brightest first, so the dimmest is last: a new light takes the last place (growing the list if there is room)
* and moves up past every dimmer one. Strictly dimmer, so an equal one already kept stays ahead.
*/
void keepBrightestLight(BrightestLights& kept, int capacity, int light, float strength) {
    if (strength <= 0.0f || capacity <= 0) {
        return;
    }

    const int room = std::min(capacity, maxLightsPerHit);

    if (kept.count == room && strength <= kept.strengths[room - 1]) {
        return;
    }

    int slot = kept.count < room ? kept.count++ : room - 1;

    while (slot > 0 && kept.strengths[slot - 1] < strength) {
        kept.strengths[slot] = kept.strengths[slot - 1];
        kept.lights[slot] = kept.lights[slot - 1];
        --slot;
    }

    kept.strengths[slot] = strength;
    kept.lights[slot] = light;
}

ProbeHitLights probeHitLights(const GpuLights& lights, const Vec3& cameraOrigin, const ProbeVolume& volume, bool keepFastApart) {
    const float reach = probeMaxDistance(volume.spacing);
    const Vec3 far = volume.origin + Vec3{volume.spacing.x * static_cast<float>(volume.counts.x - 1), volume.spacing.y * static_cast<float>(volume.counts.y - 1),
                                          volume.spacing.z * static_cast<float>(volume.counts.z - 1)};
    const Vec3 grow{reach, reach, reach};

    // The box measured from the camera, as the lights' positions are.
    const Aabb box{volume.origin - grow - cameraOrigin, far + grow - cameraOrigin};
    ProbeHitLights listed;

    const auto touches = [&](const float positionRange[4]) {
        return sphereTouchesBox(Vec3{positionRange[0], positionRange[1], positionRange[2]}, positionRange[3], box);
    };

    const auto list = [&](std::size_t slot, const std::vector<bool>& fastFlags) {
        const bool fast = keepFastApart && slot < fastFlags.size() && fastFlags[slot];
        listed.lights.push_back(GpuProbeHitLight{static_cast<std::int32_t>(slot), fast ? 1 : 0});
        listed.fast += fast ? 1 : 0;
    };

    for (std::size_t i = 0; i < lights.points.size(); ++i) {
        if (touches(lights.points[i].positionRange)) {
            list(i, lights.fastPoints);
            ++listed.points;
        }
    }

    for (std::size_t i = 0; i < lights.spots.size(); ++i) {
        if (touches(lights.spots[i].positionRange)) {
            list(i, lights.fastSpots);
            ++listed.spots;
        }
    }

    return listed;
}

void packProbeSampling(GpuFrameData& frameData, const ProbeSampling& sampling) {
    frameData.probeShading[0] = sampling.selfShadowBias;
    frameData.probeShading[1] = sampling.bounceEnergy;
    const ProbeAtlasGrid atlas = probeAtlasGrid(sampling.probes);
    frameData.probeAtlas[0] = atlas.columns;
    frameData.probeAtlas[1] = atlas.rows;
    frameData.probeAtlas[3] = sampling.cascades;
    frameData.probeLights[0] = sampling.hitPoints;
    frameData.probeLights[1] = sampling.hitSpots;
    frameData.probeLights[2] = std::clamp(sampling.lightsPerHit, 0, maxLightsPerHit);
}
