#ifndef SCENE_SKY_VISIBILITY_GLSL
#define SCENE_SKY_VISIBILITY_GLSL

// The sky visibility grid as the frame's scene set holds it (render/gpu/sky_visibility.h): every cascade's points one
// after another along z, each three texels along x (set 1 binding 29, exact texels), and the cascades' records,
// measured from the camera (binding 30), read at a point as sampleSkyVisibility does. Needs frame_data.glsl, whose
// skyVisibilityOptions say whether and how it is read. Included by scene_surface.glsl and fog_light.comp.

#include "probe_cascades.glsl"
#include "sky_visibility.glsl"

layout(set = 1, binding = 29) uniform usampler3D skyVisibilityCells;

layout(std430, set = 1, binding = 30) readonly buffer SkyVisibilityCascades {
    ProbeCascade skyVisibilityCascades[];
};

// The sky a point sees and the bounce light round it (SkySample, without how much was known).
struct SkySample {
    SkyVisibility visibility;
    SkyBounce bounce;
};

bool skyVisibilityRead() {
    return skyVisibilityOptions.x > 0.5;
}

SkyVisibility weightedSkyBands(SkyVisibility sum, SkyVisibility value, float weight) {
    return SkyVisibility(sum.open + value.open * weight, sum.lean + value.lean * weight);
}

SkyVisibility scaledSkyBands(SkyVisibility value, float factor) {
    return SkyVisibility(value.open * factor, value.lean * factor);
}

SkySample noSkySample() {
    SkyVisibility none = SkyVisibility(0.0, vec3(0.0));
    return SkySample(none, SkyBounce(none, none, none));
}

// sum + value times weight, the sky's bands and each bounce channel's.
SkySample weightedSkySample(SkySample sum, SkySample value, float weight) {
    return SkySample(weightedSkyBands(sum.visibility, value.visibility, weight),
                     SkyBounce(weightedSkyBands(sum.bounce.red, value.bounce.red, weight), weightedSkyBands(sum.bounce.green, value.bounce.green, weight),
                               weightedSkyBands(sum.bounce.blue, value.bounce.blue, weight)));
}

// One point of the grid: its three texels along x.
SkyCell skyCellAt(ivec3 texel) {
    ivec3 first = ivec3(texel.x * SKY_CELL_TEXELS, texel.yz);
    return unpackSkyCell(texelFetch(skyVisibilityCells, first, 0), texelFetch(skyVisibilityCells, first + ivec3(1, 0, 0), 0),
                         texelFetch(skyVisibilityCells, first + ivec3(2, 0, 0), 0));
}

// One cascade's blend at a point (sampleCascade): false when no point round it is baked, out of the walls and in sight.
bool skyVisibilityCascadeAt(int index, vec3 point, vec3 normal, vec3 toViewer, out SkySample blend) {
    ProbeCascade cascade = skyVisibilityCascades[index];
    vec3 spacing = cascade.spacing.xyz;
    ivec3 counts = cascade.counts.xyz;
    vec3 biased = point + probeSelfShadowBias(normal, toViewer, spacing, skyVisibilityOptions.z);
    ProbeCell cell = probeCell(cascade.origin.xyz, spacing, counts, biased);
    SkySample sum = noSkySample();
    float total = 0.0;

    for (int corner = 0; corner < 8; ++corner) {
        ivec3 grid = probeCellCorner(cell, counts, corner);
        ivec3 stored = (grid + cascade.scroll.xyz) % counts;
        SkyCell sky = skyCellAt(stored + ivec3(0, 0, index * counts.z));

        if (sky.state != SKY_VISIBILITY_BAKED) {
            continue;
        }

        vec3 position = cascade.origin.xyz + spacing * vec3(grid);

        if (!skyCornerSees(sky.reachPositive, sky.reachNegative, biased - position, spacing.x)) {
            continue;
        }

        float weight = crushProbeWeight(probeBackFaceWeight(normal, position - point)) * probeTrilinearWeight(cell.alpha, corner);
        sum = weightedSkySample(sum, SkySample(sky.visibility, sky.bounce), weight);
        total += weight;
    }

    float share = 1.0 / max(total, 1e-30);
    blend = SkySample(scaledSkyBands(sum.visibility, share),
                      SkyBounce(scaledSkyBands(sum.bounce.red, share), scaledSkyBands(sum.bounce.green, share), scaledSkyBands(sum.bounce.blue, share)));
    return total > 0.0;
}

// The sky's reach and the bounce at a point measured from the camera (a zero normal and way to the eye for a point in
// the air): each cascade, densest first, adds its blend times its weight times what the denser ones left; the rest is
// open field (SKY_OPEN_FIELD_*: the sky above, the ground below) with nothing bounced.
SkySample skyVisibilityAt(vec3 point, vec3 normal, vec3 toViewer) {
    SkySample sum = noSkySample();
    float total = 0.0;
    int cascades = int(skyVisibilityOptions.x + 0.5);

    for (int index = 0; index < cascades && total < 1.0; ++index) {
        float weight = cascadeWeight(skyVisibilityCascades[index], point) * (1.0 - total);
        SkySample blend;

        if (weight <= 0.0 || !skyVisibilityCascadeAt(index, point, normal, toViewer, blend)) {
            continue;
        }

        sum = weightedSkySample(sum, blend, weight);
        total += weight;
    }

    sum.visibility.open += float(SKY_OPEN_FIELD_OPEN) * (1.0 - total);
    sum.visibility.lean.y += float(SKY_OPEN_FIELD_LEAN_UP) * (1.0 - total);
    return sum;
}

// The light from all around the air at a point gets, as a share of the open sky's per colour channel (1 while the grid
// is not read).
vec3 skyVisibilityInAirAt(vec3 point) {
    if (!skyVisibilityRead()) {
        return vec3(1.0);
    }

    SkySample sky = skyVisibilityAt(point, vec3(0.0), vec3(0.0));
    return skyLightInAir(sky.visibility, sky.bounce, skyVisibilityOptions.y);
}

#endif
