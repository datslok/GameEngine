// The light probes' update (render/gpu/ddgi.h), shared by ddgi_update_irradiance.comp and ddgi_update_distance.comp,
// which define PROBE_TEXELS (texels per side of a probe's square, without its border) and UPDATE_IRRADIANCE (1 for the
// irradiance atlases, 0 for the distances) before including it.
//
// One workgroup per probe, one invocation per texel of its square [2021]: the probe's rays (their directions and what
// they brought back) are loaded once into shared memory, then each invocation averages all of them with its texel's
// weights, blends that into the stored value and writes it back in place; once the whole square is done, the border
// is copied from it, so filtering across the square's folded edges reads the right neighbours. The irradiance update
// does this for two atlases at once from the same rays: the still lights' and the fast lights', each with its own share
// kept (render/gpu/ddgi.h, GlobalIlluminationSettings::fastIrradianceTimeConstant). With the adaptive memory on (the
// volume's change threshold above 0), the still lights' texels blend by adaptiveProbeBlend instead, their history in the
// atlas's alpha and the change window's mean and noise in a third image (alpha is 0 otherwise, so switching the adaptive
// memory on starts every texel from its stored light). A probe a scene event restarted (render/gpu/ddgi.h,
// advanceProbeRestarts) checks whether its light really changed and restarts only then (adaptiveProbeUpdate, with the
// adaptive memory), or with the fixed memory and for the distances restarts outright, keeping the share the restart
// buffer holds. The fast atlas restarts with the still irradiance, outright.
#ifndef DDGI_UPDATE_GLSL
#define DDGI_UPDATE_GLSL

#include "ddgi.glsl"
#include "ddgi_volume.glsl"
#include "probe_relocation.glsl"

layout(local_size_x = PROBE_TEXELS, local_size_y = PROBE_TEXELS) in;

// What ddgi_trace.comp wrote: the stride's room of rays per probe, and which probes a fast light reached (which traced
// the fast count of rays); and each probe's own turn of its rays, to find their directions again.
layout(std430, set = 0, binding = 0) readonly buffer ProbeRays {
    vec4 probeRays[];
};

layout(std430, set = 0, binding = 1) readonly buffer FastProbeRays {
    uvec2 fastProbeRays[];
};

layout(std430, set = 0, binding = 2) readonly buffer ProbeNearFastLights {
    int probeNearFastLight[];
};

layout(std430, set = 0, binding = 3) readonly buffer ProbeTurns {
    ProbeTurn probeTurns[];
};

// Each probe's scene event restarts (ProbeRestart).
layout(std430, set = 0, binding = 4) readonly buffer ProbeRestarts {
    ProbeRestart probeRestarts[];
};

// Each probe's state (render/gpu/probe_relocation.h): off and sleeping probes traced nothing and keep what they hold.
layout(std430, set = 0, binding = 5) readonly buffer ProbeStates {
    ProbeState probeStates[];
};

#if UPDATE_IRRADIANCE
layout(set = 0, binding = 6, rgba16f) uniform image2D atlas;
layout(set = 0, binding = 7, rgba16f) uniform image2D fastAtlas;
layout(set = 0, binding = 8, rgba16f) uniform image2D changeAtlas;
#else
layout(set = 0, binding = 6, rg16f) uniform image2D atlas;
#endif

shared vec3 rayDirections[DDGI_MAX_RAYS];
shared vec4 rayResults[DDGI_MAX_RAYS];
shared vec4 square[PROBE_TEXELS][PROBE_TEXELS];

#if UPDATE_IRRADIANCE
shared vec3 fastRayResults[DDGI_MAX_RAYS];
shared vec4 fastSquare[PROBE_TEXELS][PROBE_TEXELS];
#endif

void main() {
    int probe = int(gl_WorkGroupID.x);

    // The whole workgroup leaves together, before any barrier.
    if (!probeTraced(probeStates[probe].state)) {
        return;
    }

    int stride = volume.counts.w;
    int rays = volumeRayCount(probeNearFastLight[probe] != 0);
    int invocations = PROBE_TEXELS * PROBE_TEXELS;

    for (int ray = int(gl_LocalInvocationIndex); ray < rays; ray += invocations) {
        rayDirections[ray] = volumeRayDirection(ray, rays, probeTurns[probe]);
        rayResults[ray] = probeRays[probe * stride + ray];
#if UPDATE_IRRADIANCE
        fastRayResults[ray] = unpackFastRayLight(fastProbeRays[probe * stride + ray]);
#endif
    }

    barrier();

    ivec2 texel = ivec2(gl_LocalInvocationID.xy);
    vec3 texelDirection = probeTexelDirection(texel, PROBE_TEXELS);
    ivec2 origin = probeTileOrigin(volume.counts.xyz, probe, PROBE_TEXELS);
    vec4 stored = imageLoad(atlas, origin + 1 + texel);
    vec4 sum = vec4(0.0);
#if UPDATE_IRRADIANCE
    vec4 fastStored = imageLoad(fastAtlas, origin + 1 + texel);
    vec3 fastSum = vec3(0.0);
#endif

    for (int ray = 0; ray < rays; ++ray) {
#if UPDATE_IRRADIANCE
        float weight = irradianceRayWeight(texelDirection, rayDirections[ray]);
        sum += vec4(rayResults[ray].rgb, 1.0) * weight;
        fastSum += fastRayResults[ray] * weight;
#else
        float distance = distanceForUpdate(rayResults[ray].a, volume.spacing.w);
        sum += vec4(distance, distance * distance, 0.0, 1.0) * distanceRayWeight(texelDirection, rayDirections[ray]);
#endif
    }

    // No ray came near this texel's direction: keep what it had.
    vec4 result = stored;
#if UPDATE_IRRADIANCE
    vec4 fastResult = fastStored;
#endif

    ProbeRestart restart = probeRestarts[probe];

    if (sum.w > DDGI_MIN_TEXEL_WEIGHT) {
#if UPDATE_IRRADIANCE
        if (volume.sky.w > 0.0) {
            vec4 change = imageLoad(changeAtlas, origin + 1 + texel);
            AdaptiveProbeTexel adaptive = adaptiveProbeUpdate(AdaptiveProbeTexel(stored.rgb, stored.a, change.rgb, change.a), sum.rgb / sum.w,
                                                              volume.hysteresis.x, volume.rotation[2].w, volume.sky.w, restart.frames.x,
                                                              restart.check);
            result = vec4(adaptive.stored, adaptive.history);
            imageStore(changeAtlas, origin + 1 + texel, vec4(adaptive.recent, adaptive.noise));
        } else {
            result = vec4(blendProbeValue(stored.rgb, sum.rgb / sum.w, restart.frames.x >= 0 ? restart.keeps.x : volume.hysteresis.x), 0.0);
        }
        fastResult = vec4(blendProbeValue(fastStored.rgb, fastSum / sum.w, restart.frames.x >= 0 ? restart.check.w : volume.origin.w), 1.0);
#else
        result = vec4(blendProbeValue(stored.rg, sum.rg / sum.w, restart.frames.y >= 0 ? restart.keeps.y : volume.hysteresis.y), 0.0, 1.0);
#endif
    }

    imageStore(atlas, origin + 1 + texel, result);
    square[texel.y][texel.x] = result;
#if UPDATE_IRRADIANCE
    imageStore(fastAtlas, origin + 1 + texel, fastResult);
    fastSquare[texel.y][texel.x] = fastResult;
#endif

    barrier();

    // Enough invocations to copy every border texel at once (probeBorderTexelCount <= PROBE_TEXELS squared).
    int number = int(gl_LocalInvocationIndex);

    if (number < probeBorderTexelCount(PROBE_TEXELS)) {
        ivec2 bordered = probeBorderTexel(number, PROBE_TEXELS);
        ivec2 source = probeBorderSource(bordered, PROBE_TEXELS) - 1;
        imageStore(atlas, origin + bordered, square[source.y][source.x]);
#if UPDATE_IRRADIANCE
        imageStore(fastAtlas, origin + bordered, fastSquare[source.y][source.x]);
#endif
    }
}

#endif
