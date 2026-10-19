// The light probes' update (render/gpu/ddgi.h), shared by ddgi_update_irradiance.comp and ddgi_update_distance.comp,
// which define PROBE_TEXELS (texels per side of a probe's square, without its border) and UPDATE_IRRADIANCE (1 for the
// irradiance atlas, 0 for the distances) before including it.
//
// One workgroup per probe, one invocation per texel of its square [2021]: the probe's rays (their directions and what
// they brought back) are loaded once into shared memory, then each invocation averages all of them with its texel's
// weights, blends that into the stored value and writes it back in place; once the whole square is done, the border
// is copied from it, so filtering across the square's folded edges reads the right neighbours.
#ifndef DDGI_UPDATE_GLSL
#define DDGI_UPDATE_GLSL

#include "ddgi.glsl"
#include "ddgi_volume.glsl"

layout(local_size_x = PROBE_TEXELS, local_size_y = PROBE_TEXELS) in;

layout(std430, set = 0, binding = 0) readonly buffer ProbeRays {
    vec4 probeRays[];
};

#if UPDATE_IRRADIANCE
layout(set = 0, binding = 1, rgba16f) uniform image2D atlas;
#else
layout(set = 0, binding = 1, rg16f) uniform image2D atlas;
#endif

shared vec3 rayDirections[DDGI_MAX_RAYS];
shared vec4 rayResults[DDGI_MAX_RAYS];
shared vec4 square[PROBE_TEXELS][PROBE_TEXELS];

void main() {
    int probe = int(gl_WorkGroupID.x);
    int rays = volume.counts.w;
    int invocations = PROBE_TEXELS * PROBE_TEXELS;

    for (int ray = int(gl_LocalInvocationIndex); ray < rays; ray += invocations) {
        rayDirections[ray] = volumeRayDirection(ray);
        rayResults[ray] = probeRays[probe * rays + ray];
    }

    barrier();

    ivec2 texel = ivec2(gl_LocalInvocationID.xy);
    vec3 texelDirection = probeTexelDirection(texel, PROBE_TEXELS);
    ivec2 origin = probeTileOrigin(volume.counts.xyz, probe, PROBE_TEXELS);
    vec4 stored = imageLoad(atlas, origin + 1 + texel);
    vec4 sum = vec4(0.0);

    for (int ray = 0; ray < rays; ++ray) {
#if UPDATE_IRRADIANCE
        sum += vec4(rayResults[ray].rgb, 1.0) * irradianceRayWeight(texelDirection, rayDirections[ray]);
#else
        float distance = distanceForUpdate(rayResults[ray].a, volume.spacing.w);
        sum += vec4(distance, distance * distance, 0.0, 1.0) * distanceRayWeight(texelDirection, rayDirections[ray]);
#endif
    }

    // No ray came near this texel's direction: keep what it had.
    vec4 result = stored;

    if (sum.w > DDGI_MIN_TEXEL_WEIGHT) {
#if UPDATE_IRRADIANCE
        vec3 fresh = encodeIrradiance(sum.rgb / sum.w);
        float hysteresis = irradianceTexelHysteresis(volume.hysteresis.x, stored.rgb, fresh);
        result = vec4(blendProbeValue(stored.rgb, fresh, hysteresis), 1.0);
#else
        result = vec4(blendProbeValue(stored.rg, sum.rg / sum.w, volume.hysteresis.y), 0.0, 1.0);
#endif
    }

    imageStore(atlas, origin + 1 + texel, result);
    square[texel.y][texel.x] = result;

    barrier();

    // Enough invocations to copy every border texel at once (probeBorderTexelCount <= PROBE_TEXELS squared).
    int number = int(gl_LocalInvocationIndex);

    if (number < probeBorderTexelCount(PROBE_TEXELS)) {
        ivec2 bordered = probeBorderTexel(number, PROBE_TEXELS);
        ivec2 source = probeBorderSource(bordered, PROBE_TEXELS) - 1;
        imageStore(atlas, origin + bordered, square[source.y][source.x]);
    }
}

#endif
