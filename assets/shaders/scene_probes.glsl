// The light probes' atlases in set 1 (render/gpu/ddgi_sampling.h), and sampling them at a surface: the corner loop that
// reads the atlases around ddgi_sampling.glsl's weights, and the blend of the cascades (render/gpu/probe_cascades.h)
// densest first. Included by triangle.frag, ddgi_trace.comp and ddgi_seed.comp, after frame_data.glsl (the probes'
// numbers are in the frame's uniform block). Mirrored by sampleProbes and sampleProbeCascades in C++. Points are
// measured from the camera, as the cascades are.
#ifndef SCENE_PROBES_GLSL
#define SCENE_PROBES_GLSL

#include "ddgi_sampling.glsl"
#include "scene_probe_cascades.glsl"
#include "scene_probe_states.glsl"
#include "scene_sky.glsl"

// fogAmbient, for the light from all around a point in the air.
#include "volumetric_fog.glsl"

// Bound to an empty stand-in while the probes are not sampled (nothing reads them then).
layout(set = 1, binding = 12) uniform sampler2D probeIrradianceAtlas;
layout(set = 1, binding = 13) uniform sampler2D probeDistanceAtlas;
layout(set = 1, binding = 15) uniform sampler2D probeFastIrradianceAtlas;

// What the probes give a surface: irradiance for its diffuse part, the same read in the mirror direction for its specular part.
struct ProbeLight {
    vec3 diffuse;
    vec3 specular;
};

// The still lights' atlas and the fast lights', each blended on its own with the same weights: the screen adds them, and
// a probe ray's hit passes each on into its own atlas.
struct ProbeLights {
    ProbeLight still;
    ProbeLight fast;
};

// The atlases' columns and rows of squares, every cascade's probes one after another (ddgi.h, probeAtlasGrid).
ivec2 probeAtlasGrid() {
    return probeAtlas.xy;
}

/*
* Cascade number cascade's light at a point with this normal, seen from toViewer (for a probe ray's hit, back along the
* ray): the 8 corner probes of the cell round the biased point, each read in the right direction and weighted.
* lookupNormal is the direction the irradiance is read in, reflected the mirror direction (read only withSpecular, a
* constant at each call, so a matte caller pays nothing for it). Only traced probes count, each where relocation moved
* it; traced is false when none round the point is.
*/
ProbeLights sampleProbes(int cascade, vec3 point, vec3 normal, vec3 lookupNormal, vec3 reflected, vec3 toViewer, bool withSpecular, out bool traced) {
    ProbeCascade window = probeCascades[cascade];
    vec3 origin = window.origin.xyz;
    vec3 spacing = window.spacing.xyz;
    ivec3 counts = window.counts.xyz;
    ivec2 atlas = probeAtlasGrid();
    vec3 biased = point + probeSelfShadowBias(normal, toViewer, spacing, probeShading.x);
    ProbeCell cell = probeCell(origin, spacing, counts, biased);
    ProbeBlend diffuse = ProbeBlend(vec3(0.0), 0.0);
    ProbeBlend specular = ProbeBlend(vec3(0.0), 0.0);
    ProbeBlend fastDiffuse = ProbeBlend(vec3(0.0), 0.0);
    ProbeBlend fastSpecular = ProbeBlend(vec3(0.0), 0.0);

    for (int corner = 0; corner < 8; ++corner) {
        ivec3 grid = probeCellCorner(cell, counts, corner);
        int index = cascadeProbeNumber(window, grid);
        ProbeState state = probeStates[index];

        if (!probeTraced(state.state)) {
            continue;
        }

        vec3 probe = origin + spacing * vec3(grid) + state.offset;
        vec3 fromProbe = biased - probe;
        float distance = length(fromProbe);
        vec3 direction = distance > 0.0 ? fromProbe / distance : normal;

        vec2 moments = textureLod(probeDistanceAtlas, probeAtlasUv(atlas, index, direction, DDGI_DISTANCE_TEXELS), 0.0).rg;
        float weight = probeCornerWeight(normal, probe - point, distance, moments, probeTrilinearWeight(cell.alpha, corner));
        vec2 lookupUv = probeAtlasUv(atlas, index, lookupNormal, DDGI_IRRADIANCE_TEXELS);
        addProbeSample(diffuse, textureLod(probeIrradianceAtlas, lookupUv, 0.0).rgb, weight);
        addProbeSample(fastDiffuse, textureLod(probeFastIrradianceAtlas, lookupUv, 0.0).rgb, weight);

        if (withSpecular) {
            vec2 reflectedUv = probeAtlasUv(atlas, index, reflected, DDGI_IRRADIANCE_TEXELS);
            addProbeSample(specular, textureLod(probeIrradianceAtlas, reflectedUv, 0.0).rgb, weight);
            addProbeSample(fastSpecular, textureLod(probeFastIrradianceAtlas, reflectedUv, 0.0).rgb, weight);
        }
    }

    traced = diffuse.weight > 0.0;

    if (!traced) {
        return ProbeLights(ProbeLight(vec3(0.0), vec3(0.0)), ProbeLight(vec3(0.0), vec3(0.0)));
    }

    ProbeLight still = ProbeLight(finishProbeBlend(diffuse), withSpecular ? finishProbeBlend(specular) : vec3(0.0));
    ProbeLight fast = ProbeLight(finishProbeBlend(fastDiffuse), withSpecular ? finishProbeBlend(fastSpecular) : vec3(0.0));
    return ProbeLights(still, fast);
}

/*
* The light from all around at a point: the cascades' from fromCascade on, densest first, each weighted by its weight
* times what the denser ones left, until that reaches 1; a cascade with no traced probe round the point leaves its share
* to the coarser ones; the flat ambient takes whatever is left for still light, and fast light fades out with the
* cascades. Frames without probes never get here (the main pass's probe pipelines only).
*/
ProbeLights probeAmbient(vec3 point, vec3 normal, vec3 lookupNormal, vec3 reflected, vec3 toViewer, int fromCascade, bool withSpecular) {
    ProbeLights sum = ProbeLights(ProbeLight(vec3(0.0), vec3(0.0)), ProbeLight(vec3(0.0), vec3(0.0)));
    float total = 0.0;

    for (int cascade = fromCascade; cascade < probeAtlas.w && total < 1.0; ++cascade) {
        float weight = cascadeWeight(probeCascades[cascade], point) * (1.0 - total);

        if (weight <= 0.0) {
            continue;
        }

        bool traced;
        ProbeLights light = sampleProbes(cascade, point, normal, lookupNormal, reflected, toViewer, withSpecular, traced);

        if (!traced) {
            continue;
        }

        sum.still.diffuse += light.still.diffuse * weight;
        sum.still.specular += light.still.specular * weight;
        sum.fast.diffuse += light.fast.diffuse * weight;
        sum.fast.specular += light.fast.specular * weight;
        total += weight;
    }

    sum.still.diffuse += flatAmbient() * (1.0 - total);
    sum.still.specular += flatAmbient() * (1.0 - total);
    return sum;
}

/*
* Cascade number cascade's light from all around a point in the air (volumetric fog; render/gpu/volumetric_fog.h): the 8
* corner probes weighted by nearness and visibility alone (no surface faces any of them, probeAirWeight), each read
* straight up and straight down, the still and fast lights' atlases blended apart and summed. traced is false when no
* traced probe is round the point.
*/
vec3 sampleProbesInAir(int cascade, vec3 point, out bool traced) {
    ProbeCascade window = probeCascades[cascade];
    vec3 origin = window.origin.xyz;
    vec3 spacing = window.spacing.xyz;
    ivec3 counts = window.counts.xyz;
    ivec2 atlas = probeAtlasGrid();
    ProbeCell cell = probeCell(origin, spacing, counts, point);
    ProbeBlend up = ProbeBlend(vec3(0.0), 0.0);
    ProbeBlend down = ProbeBlend(vec3(0.0), 0.0);
    ProbeBlend fastUp = ProbeBlend(vec3(0.0), 0.0);
    ProbeBlend fastDown = ProbeBlend(vec3(0.0), 0.0);

    for (int corner = 0; corner < 8; ++corner) {
        ivec3 grid = probeCellCorner(cell, counts, corner);
        int index = cascadeProbeNumber(window, grid);
        ProbeState state = probeStates[index];

        if (!probeTraced(state.state)) {
            continue;
        }

        vec3 fromProbe = point - (origin + spacing * vec3(grid) + state.offset);
        float distance = length(fromProbe);
        vec3 direction = distance > 0.0 ? fromProbe / distance : vec3(0.0, 1.0, 0.0);

        vec2 moments = textureLod(probeDistanceAtlas, probeAtlasUv(atlas, index, direction, DDGI_DISTANCE_TEXELS), 0.0).rg;
        float weight = probeAirWeight(distance, moments, probeTrilinearWeight(cell.alpha, corner));
        vec2 upUv = probeAtlasUv(atlas, index, vec3(0.0, 1.0, 0.0), DDGI_IRRADIANCE_TEXELS);
        vec2 downUv = probeAtlasUv(atlas, index, vec3(0.0, -1.0, 0.0), DDGI_IRRADIANCE_TEXELS);
        addProbeSample(up, textureLod(probeIrradianceAtlas, upUv, 0.0).rgb, weight);
        addProbeSample(down, textureLod(probeIrradianceAtlas, downUv, 0.0).rgb, weight);
        addProbeSample(fastUp, textureLod(probeFastIrradianceAtlas, upUv, 0.0).rgb, weight);
        addProbeSample(fastDown, textureLod(probeFastIrradianceAtlas, downUv, 0.0).rgb, weight);
    }

    traced = up.weight > 0.0;

    if (!traced) {
        return vec3(0.0);
    }

    return fogAmbient(finishProbeBlend(up) + finishProbeBlend(fastUp), finishProbeBlend(down) + finishProbeBlend(fastDown));
}

/*
* The light from all around a point in the air: the cascades blended densest first as for a surface (probeAmbient), the
* flat ambient taking whatever share is left. Frames without probes have no cascades, so the flat ambient alone.
*/
vec3 probeAmbientInAir(vec3 point) {
    vec3 sum = vec3(0.0);
    float total = 0.0;

    for (int cascade = 0; cascade < probeAtlas.w && total < 1.0; ++cascade) {
        float weight = cascadeWeight(probeCascades[cascade], point) * (1.0 - total);

        if (weight <= 0.0) {
            continue;
        }

        bool traced;
        vec3 light = sampleProbesInAir(cascade, point, traced);

        if (!traced) {
            continue;
        }

        sum += light * weight;
        total += weight;
    }

    return sum + flatAmbient() * (1.0 - total);
}

#endif
