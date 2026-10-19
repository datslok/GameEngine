#ifndef DDGI_SAMPLING_GLSL
#define DDGI_SAMPLING_GLSL

// Reading the light probes back at a surface, and the point and spot lights a probe ray's hit adds. Pure functions,
// mirrored by render/gpu/ddgi_sampling.h, which explains each; the GPU tests check them against it. The atlas reads
// themselves live in scene_probes.glsl.

#include "render/gpu/bloom_shared.h"
#include "render/gpu/ddgi_shared.h"
#include "ddgi.glsl"
#include "lighting.glsl"

vec3 probeSelfShadowBias(vec3 normal, vec3 toViewer, vec3 spacing, float selfShadowBias) {
    float smallestSpacing = min(spacing.x, min(spacing.y, spacing.z));
    return (normal * DDGI_BIAS_NORMAL_SHARE + toViewer * DDGI_BIAS_VIEW_SHARE) * (DDGI_BIAS_SPACING_SHARE * smallestSpacing * selfShadowBias);
}

struct ProbeCell {
    ivec3 base;
    vec3 alpha;
};

ProbeCell probeCell(vec3 origin, vec3 spacing, ivec3 counts, vec3 point) {
    vec3 cells = (point - origin) / spacing;
    ivec3 base = clamp(ivec3(floor(cells)), ivec3(0), counts - 1);
    return ProbeCell(base, clamp(cells - vec3(base), 0.0, 1.0));
}

ivec3 probeCellCorner(ProbeCell cell, ivec3 counts, int corner) {
    return min(cell.base + ivec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1), counts - 1);
}

float probeTrilinearWeight(vec3 alpha, int corner) {
    vec3 factors = mix(1.0 - alpha, alpha, vec3(ivec3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1)));
    factors = max(factors, vec3(DDGI_MIN_TRILINEAR));
    return factors.x * factors.y * factors.z;
}

float probeBackFaceWeight(vec3 normal, vec3 toProbe) {
    float probeDistance = length(toProbe);
    float facing = probeDistance > 0.0 ? dot(normal, toProbe) / probeDistance : 1.0;
    float wrapped = (facing + 1.0) * 0.5;
    return wrapped * wrapped + DDGI_WRAP_FLOOR;
}

float probeVisibility(float distance, vec2 moments) {
    float mean = moments.x;

    if (distance <= mean) {
        return 1.0;
    }

    float variance = abs(moments.y - mean * mean);
    float beyond = distance - mean;
    float bound = variance / (variance + beyond * beyond);
    return max(bound * bound * bound, DDGI_MIN_VISIBILITY);
}

float crushProbeWeight(float weight) {
    float kept = max(weight, DDGI_SMALLEST_WEIGHT);

    if (kept < DDGI_CRUSH_THRESHOLD) {
        return kept * kept * kept / (DDGI_CRUSH_THRESHOLD * DDGI_CRUSH_THRESHOLD);
    }

    return kept;
}

float probeCornerWeight(vec3 normal, vec3 toProbe, float distance, vec2 moments, float trilinear) {
    return crushProbeWeight(probeBackFaceWeight(normal, toProbe) * probeVisibility(distance, moments)) * trilinear;
}

float probeVolumeWeight(vec3 origin, vec3 spacing, ivec3 counts, vec3 point) {
    vec3 cells = (point - origin) / spacing;
    vec3 inside = min(cells, vec3(counts - 1) - cells);
    vec3 weights = clamp((inside + (DDGI_VOLUME_MARGIN + DDGI_VOLUME_FADE)) / DDGI_VOLUME_FADE, 0.0, 1.0);
    return weights.x * weights.y * weights.z;
}

struct ProbeBlend {
    vec3 sum;
    float weight;
};

void addProbeSample(inout ProbeBlend blend, vec3 encodedIrradiance, float weight) {
    blend.sum += pow(encodedIrradiance, vec3(DDGI_IRRADIANCE_POWER / 2.0)) * weight;
    blend.weight += weight;
}

vec3 finishProbeBlend(ProbeBlend blend) {
    vec3 root = blend.sum / blend.weight;
    return root * root;
}

vec3 pointLightAtHit(vec3 offsetToLight, float range, vec3 radiance, float sourceRadius, vec3 normal) {
    float distance = length(offsetToLight);

    if (distance <= 0.0 || distance >= range) {
        return vec3(0.0);
    }

    vec3 toLight = offsetToLight / distance;
    return radiance * distanceFalloff(distance, range, sourceRadius) * max(0.0, dot(normal, toLight));
}

vec3 spotLightAtHit(vec3 offsetToLight, float range, vec3 radiance, float sourceRadius, vec3 beamDirection, float cosOuter, float cosInner,
                    vec3 normal) {
    float distance = length(offsetToLight);

    if (distance <= 0.0 || distance >= range) {
        return vec3(0.0);
    }

    vec3 toLight = offsetToLight / distance;
    float cone = spotCone(dot(-toLight, beamDirection), cosOuter, cosInner);
    return radiance * (distanceFalloff(distance, range, sourceRadius) * cone) * max(0.0, dot(normal, toLight));
}

float hitLightStrength(vec3 irradiance) {
    return dot(irradiance, vec3(BLOOM_LUMA_RED, BLOOM_LUMA_GREEN, BLOOM_LUMA_BLUE));
}

struct BrightestLights {
    int lights[DDGI_MAX_HIT_LIGHTS];
    float strengths[DDGI_MAX_HIT_LIGHTS];
    int count;
};

void keepBrightestLight(inout BrightestLights kept, int capacity, int light, float strength) {
    if (strength <= 0.0 || capacity <= 0) {
        return;
    }

    int room = min(capacity, DDGI_MAX_HIT_LIGHTS);

    if (kept.count == room && strength <= kept.strengths[room - 1]) {
        return;
    }

    int slot = kept.count < room ? kept.count : room - 1;
    kept.count = min(kept.count + 1, room);

    while (slot > 0 && kept.strengths[slot - 1] < strength) {
        kept.strengths[slot] = kept.strengths[slot - 1];
        kept.lights[slot] = kept.lights[slot - 1];
        --slot;
    }

    kept.strengths[slot] = strength;
    kept.lights[slot] = light;
}

#endif
