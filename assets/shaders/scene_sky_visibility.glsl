#ifndef SCENE_SKY_VISIBILITY_GLSL
#define SCENE_SKY_VISIBILITY_GLSL

// The sky visibility grid as the frame's scene set holds it (render/gpu/sky_visibility.h): every cascade's points one
// after another along z, each SKY_CELL_TEXELS texels along x (set 1 binding 29, exact texels), and the cascades' records,
// measured from the camera (binding 30), read at a point as sampleSkyVisibility does. Needs frame_data.glsl, whose
// skyVisibilityOptions say whether and how it is read. Included by scene_surface.glsl and fog_light.comp.

#include "probe_cascades.glsl"
#include "sky_visibility.glsl"

layout(set = 1, binding = 29) uniform usampler3D skyVisibilityCells;

layout(std430, set = 1, binding = 30) readonly buffer SkyVisibilityCascades {
    ProbeCascade skyVisibilityCascades[];
};

// The sky a point sees and the bounce light round it (the sky's, and the sun's per unit of its irradiance), and the
// share the grid reached but had no point to read (SkySample, without how much was known).
struct SkySample {
    SkyVisibility visibility;
    SkyBounce bounce;
    SkyBounce sunBounce;
    float missing;
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
    return SkySample(none, SkyBounce(none, none, none), SkyBounce(none, none, none), 0.0);
}

// sum + value times weight, for each of a bounce's channels.
SkyBounce weightedSkyBounce(SkyBounce sum, SkyBounce value, float weight) {
    return SkyBounce(weightedSkyBands(sum.red, value.red, weight), weightedSkyBands(sum.green, value.green, weight), weightedSkyBands(sum.blue, value.blue, weight));
}

SkyBounce scaledSkyBounce(SkyBounce value, float factor) {
    return SkyBounce(scaledSkyBands(value.red, factor), scaledSkyBands(value.green, factor), scaledSkyBands(value.blue, factor));
}

// sum + value times weight, the sky's bands and each bounce channel's, the sky's and the sun's.
SkySample weightedSkySample(SkySample sum, SkySample value, float weight) {
    return SkySample(weightedSkyBands(sum.visibility, value.visibility, weight), weightedSkyBounce(sum.bounce, value.bounce, weight),
                     weightedSkyBounce(sum.sunBounce, value.sunBounce, weight), sum.missing);
}

// One point of the grid: its four texels along x.
SkyCell skyCellAt(ivec3 texel) {
    ivec3 first = ivec3(texel.x * SKY_CELL_TEXELS, texel.yz);
    return unpackSkyCell(texelFetch(skyVisibilityCells, first, 0), texelFetch(skyVisibilityCells, first + ivec3(1, 0, 0), 0),
                         texelFetch(skyVisibilityCells, first + ivec3(2, 0, 0), 0), texelFetch(skyVisibilityCells, first + ivec3(3, 0, 0), 0));
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
        sum = weightedSkySample(sum, SkySample(sky.visibility, sky.bounce, sky.sunBounce, 0.0), weight);
        total += weight;
    }

    float share = 1.0 / max(total, 1e-30);
    blend = SkySample(scaledSkyBands(sum.visibility, share), scaledSkyBounce(sum.bounce, share), scaledSkyBounce(sum.sunBounce, share), 0.0);
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

        if (weight <= 0.0) {
            continue;
        }

        if (!skyVisibilityCascadeAt(index, point, normal, toViewer, blend)) {
            sum.missing += weight;
            continue;
        }

        sum = weightedSkySample(sum, blend, weight);
        total += weight;
    }

    sum.visibility.open += float(SKY_OPEN_FIELD_OPEN) * (1.0 - total);
    sum.visibility.lean.y += float(SKY_OPEN_FIELD_LEAN_UP) * (1.0 - total);
    // A denser cascade's gap is offered to the next one too, so it may be counted twice: never more than is unknown.
    sum.missing = min(sum.missing, 1.0 - total);
    return sum;
}

// skyMissingLeftDark: the share reached but unread left dark instead of the open field, for the air.
SkySample skyMissingLeftDark(SkySample read) {
    read.visibility.open -= float(SKY_OPEN_FIELD_OPEN) * read.missing;
    read.visibility.lean.y -= float(SKY_OPEN_FIELD_LEAN_UP) * read.missing;
    return read;
}

// The light from all around the air at a point gets, as a share of the open sky's per colour channel (1 while the grid
// is not read).
vec3 skyVisibilityInAirAt(vec3 point) {
    if (!skyVisibilityRead()) {
        return vec3(1.0);
    }

    SkySample sky = skyMissingLeftDark(skyVisibilityAt(point, vec3(0.0), vec3(0.0)));
    return skyLightInAir(sky.visibility, sky.bounce, skyVisibilityOptions.y);
}

#endif
