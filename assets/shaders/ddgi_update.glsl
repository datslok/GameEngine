// The light probes' update (render/gpu/ddgi.h), shared by ddgi_update_irradiance.comp and ddgi_update_distance.comp,
// which define PROBE_TEXELS (texels per side of a probe's square, without its border) and UPDATE_IRRADIANCE (1 for the
// irradiance atlases, 0 for the distances) before including it.
//
// One workgroup per traced probe (the frame's list, render/gpu/probe_relocation.h, chooseTracedProbes), one invocation
// per texel of its square [2021]: the probe's rays (their directions and what they brought back) are loaded into shared
// memory DDGI_UPDATE_RAY_CHUNK at a time, each invocation adding each chunk into its texel's weighted sums in ray order
// (so the sums are the same however many chunks there are), then it blends the average into the stored value and
// writes it back in place; once the whole square is done, the border
// is copied from it, so filtering across the square's folded edges reads the right neighbours. The irradiance update
// does this for two atlases at once from the same rays: the still lights' and the fast lights', each with its own share
// kept (render/gpu/ddgi.h, GlobalIlluminationSettings::fastIrradianceTimeConstant). With the adaptive memory on (the
// volume's change threshold above 0), the still lights' texels blend by adaptiveProbeBlend instead, their history in the
// atlas's alpha and the change window's mean and noise in a third image (alpha is 0 otherwise, so switching the adaptive
// memory on starts every texel from its stored light). A probe a scene event restarted (render/gpu/ddgi.h,
// advanceProbeRestarts) checks whether its light really changed and restarts only then (adaptiveProbeUpdate, with the
// adaptive memory), or with the fixed memory and for the distances restarts outright, keeping the share the restart
// buffer holds. The fast atlas restarts with the still irradiance, outright. A probe that starts from a seed (the
// coarser cascades' light where it stands, ddgi_seed.comp) takes it as its stored light first (seededProbeTexel).
#ifndef DDGI_UPDATE_GLSL
#define DDGI_UPDATE_GLSL

#include "ddgi.glsl"
#include "ddgi_volume.glsl"
#include "probe_cascades.glsl"
#include "probe_relocation.glsl"

layout(local_size_x = PROBE_TEXELS, local_size_y = PROBE_TEXELS) in;

// What ddgi_trace.comp wrote: the stride's room of rays per traced probe, at its place in the frame's list, and which
// probes a fast light reached (which traced the fast count of rays); and each probe's own turn of its rays, to find their
// directions again.
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

// The probes traced this frame (as the trace read them): how many, then each one's number.
layout(std430, set = 0, binding = 4) readonly buffer TracedProbes {
    int tracedProbes[];
};

// Each probe's scene event restarts (ProbeRestart).
layout(std430, set = 0, binding = 5) readonly buffer ProbeRestarts {
    ProbeRestart probeRestarts[];
};

// Each probe's state (render/gpu/probe_relocation.h): off and sleeping probes are not in the list and keep what they hold.
layout(std430, set = 0, binding = 6) readonly buffer ProbeStates {
    ProbeState probeStates[];
};

// The cascades, for the longest distance each one's probes record.
layout(std430, set = 0, binding = 7) readonly buffer ProbeCascades {
    ProbeCascade probeCascades[];
};

#include "probe_cascade_holding.glsl"

// This frame's seeds (ddgi_seed.comp): a probe's still and fast light per texel of its square, at its seed slot.
struct ProbeSeedTexel {
    vec4 still;
    vec4 fast;
};

layout(std430, set = 0, binding = 8) readonly buffer ProbeSeeds {
    ProbeSeedTexel probeSeeds[];
};

#if UPDATE_IRRADIANCE
layout(set = 0, binding = 9, rgba16f) uniform image2D atlas;
layout(set = 0, binding = 10, rgba16f) uniform image2D fastAtlas;
layout(set = 0, binding = 11, rgba16f) uniform image2D changeAtlas;
#else
layout(set = 0, binding = 9, rg16f) uniform image2D atlas;
#endif

shared vec3 rayDirections[DDGI_UPDATE_RAY_CHUNK];
shared vec4 rayResults[DDGI_UPDATE_RAY_CHUNK];
shared vec4 square[PROBE_TEXELS][PROBE_TEXELS];

#if UPDATE_IRRADIANCE
shared vec3 fastRayResults[DDGI_UPDATE_RAY_CHUNK];
shared vec4 fastSquare[PROBE_TEXELS][PROBE_TEXELS];
#endif

void main() {
    // One workgroup per entry of the list, so every one has a probe: the whole workgroup goes on together.
    int slot = int(gl_WorkGroupID.x);
    int probe = tracedProbes[1 + slot];

    int stride = volume.counts.w;
    int rays = volumeRayCount(probeNearFastLight[probe] != 0);
    int invocations = PROBE_TEXELS * PROBE_TEXELS;

    ivec2 texel = ivec2(gl_LocalInvocationID.xy);
    vec3 texelDirection = probeTexelDirection(texel, PROBE_TEXELS);
    ivec2 origin = probeTileOrigin(volumeAtlasGrid(), probe, PROBE_TEXELS);
    ProbeRestart restart = probeRestarts[probe];
    vec4 stored = imageLoad(atlas, origin + 1 + texel);
    vec4 sum = vec4(0.0);
#if UPDATE_IRRADIANCE
    vec4 fastStored = imageLoad(fastAtlas, origin + 1 + texel);
    vec3 fastSum = vec3(0.0);
    vec4 change = imageLoad(changeAtlas, origin + 1 + texel);

    // A seed stands for the probe's first frames (render/gpu/ddgi.h, seededProbeTexel): its light, marked restarted.
    if (restart.seed.x >= 0) {
        ProbeSeedTexel seed = probeSeeds[restart.seed.x * PROBE_TEXELS * PROBE_TEXELS + int(gl_LocalInvocationIndex)];
        AdaptiveProbeTexel seeded = seededProbeTexel(seed.still.rgb);
        stored = vec4(seeded.stored, seeded.history);
        change = vec4(seeded.recent, seeded.noise);
        fastStored = vec4(seed.fast.rgb, 1.0);
    }
#else
    float longest = probeCascades[probeCascadeHolding(probe, volume.cascades.x)].origin.w;
#endif

    // The rays a chunk at a time: loaded together, then added in order by every texel; the second barrier keeps the next
    // chunk's loads from overwriting rays a texel is still adding.
    for (int first = 0; first < rays; first += DDGI_UPDATE_RAY_CHUNK) {
        int count = min(rays - first, DDGI_UPDATE_RAY_CHUNK);

        for (int loaded = int(gl_LocalInvocationIndex); loaded < count; loaded += invocations) {
            int ray = first + loaded;
            rayDirections[loaded] = volumeRayDirection(ray, rays, probeTurns[probe]);
            rayResults[loaded] = probeRays[slot * stride + ray];
#if UPDATE_IRRADIANCE
            fastRayResults[loaded] = unpackFastRayLight(fastProbeRays[slot * stride + ray]);
#endif
        }

        barrier();

        for (int ray = 0; ray < count; ++ray) {
#if UPDATE_IRRADIANCE
            float weight = irradianceRayWeight(texelDirection, rayDirections[ray]);
            sum += vec4(rayResults[ray].rgb, 1.0) * weight;
            fastSum += fastRayResults[ray] * weight;
#else
            float distance = distanceForUpdate(rayResults[ray].a, longest);
            sum += vec4(distance, distance * distance, 0.0, 1.0) * distanceRayWeight(texelDirection, rayDirections[ray]);
#endif
        }

        barrier();
    }

    // No ray came near this texel's direction: keep what it had.
    vec4 result = stored;
#if UPDATE_IRRADIANCE
    vec4 fastResult = fastStored;
#endif

    if (sum.w > DDGI_MIN_TEXEL_WEIGHT) {
#if UPDATE_IRRADIANCE
        if (volume.sky.w > 0.0) {
            AdaptiveProbeTexel adaptive = adaptiveProbeUpdate(AdaptiveProbeTexel(stored.rgb, stored.a, change.rgb, change.a), sum.rgb / sum.w,
                                                              volume.hysteresis.x, volume.rotation[2].w, volume.sky.w, restart.frames.x,
                                                              restart.check);
            result = vec4(adaptive.stored, adaptive.history);
            imageStore(changeAtlas, origin + 1 + texel, vec4(adaptive.recent, adaptive.noise));
        } else {
            result = vec4(blendProbeValue(stored.rgb, sum.rgb / sum.w, restart.frames.x >= 0 ? restart.keeps.x : volume.hysteresis.x), 0.0);
        }
        fastResult = vec4(blendProbeValue(fastStored.rgb, fastSum / sum.w, restart.frames.x >= 0 ? restart.check.w : volume.keeps.x), 1.0);
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
