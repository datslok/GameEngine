#ifndef SCENE_SKY_VISIBILITY_GLSL
#define SCENE_SKY_VISIBILITY_GLSL

// The sky visibility grid as the frame's scene set holds it (render/gpu/sky_visibility.h): every cascade's points one
// after another along z (set 1 binding 29, exact texels) and the cascades' records, measured from the camera (binding
// 30), read at a point as sampleSkyVisibility does. Needs frame_data.glsl, whose skyVisibilityOptions say whether and
// how it is read. Included by scene_surface.glsl and fog_light.comp.

#include "probe_cascades.glsl"
#include "sky_visibility.glsl"

layout(set = 1, binding = 29) uniform usampler3D skyVisibilityCells;

layout(std430, set = 1, binding = 30) readonly buffer SkyVisibilityCascades {
    ProbeCascade skyVisibilityCascades[];
};

bool skyVisibilityRead() {
    return skyVisibilityOptions.x > 0.5;
}

// One cascade's blend at a point (sampleCascade): false when no point round it is baked, out of the walls and in sight.
bool skyVisibilityCascadeAt(int index, vec3 point, vec3 normal, vec3 toViewer, out SkyVisibility blend) {
    ProbeCascade cascade = skyVisibilityCascades[index];
    vec3 spacing = cascade.spacing.xyz;
    ivec3 counts = cascade.counts.xyz;
    vec3 biased = point + probeSelfShadowBias(normal, toViewer, spacing, skyVisibilityOptions.z);
    ProbeCell cell = probeCell(cascade.origin.xyz, spacing, counts, biased);
    SkyVisibility sum = SkyVisibility(0.0, vec3(0.0));
    float total = 0.0;

    for (int corner = 0; corner < 8; ++corner) {
        ivec3 grid = probeCellCorner(cell, counts, corner);
        ivec3 stored = (grid + cascade.scroll.xyz) % counts;
        SkyCell sky = unpackSkyCell(texelFetch(skyVisibilityCells, stored + ivec3(0, 0, index * counts.z), 0));

        if (sky.state != SKY_VISIBILITY_BAKED) {
            continue;
        }

        vec3 position = cascade.origin.xyz + spacing * vec3(grid);

        if (!skyCornerSees(sky.reachPositive, sky.reachNegative, biased - position, spacing.x)) {
            continue;
        }

        float weight = crushProbeWeight(probeBackFaceWeight(normal, position - point)) * probeTrilinearWeight(cell.alpha, corner);
        sum.open += sky.visibility.open * weight;
        sum.lean += sky.visibility.lean * weight;
        total += weight;
    }

    blend = SkyVisibility(sum.open / max(total, 1e-30), sum.lean / max(total, 1e-30));
    return total > 0.0;
}

// The sky's reach at a point measured from the camera (a zero normal and way to the eye for a point in the air): each
// cascade, densest first, adds its blend times its weight times what the denser ones left; the rest is open sky.
SkyVisibility skyVisibilityAt(vec3 point, vec3 normal, vec3 toViewer) {
    SkyVisibility sum = SkyVisibility(0.0, vec3(0.0));
    float total = 0.0;
    int cascades = int(skyVisibilityOptions.x + 0.5);

    for (int index = 0; index < cascades && total < 1.0; ++index) {
        float weight = cascadeWeight(skyVisibilityCascades[index], point) * (1.0 - total);
        SkyVisibility blend;

        if (weight <= 0.0 || !skyVisibilityCascadeAt(index, point, normal, toViewer, blend)) {
            continue;
        }

        sum.open += blend.open * weight;
        sum.lean += blend.lean * weight;
        total += weight;
    }

    sum.open += 1.0 - total;
    return sum;
}

// The share of the open sky's light the air at a point gets (1 while the grid is not read).
float skyVisibilityInAirAt(vec3 point) {
    if (!skyVisibilityRead()) {
        return 1.0;
    }

    return skyVisibilityInAir(skyVisibilityAt(point, vec3(0.0), vec3(0.0)), skyVisibilityOptions.y, skyVisibilityMore.x);
}

#endif
