// Reading the sky's tables (render/gpu/atmosphere.h): the transmittance, the multiple scattering and the sky view, each
// from the sampler it is given (linear, clamped). Not part of the pure library, since it reads textures; included by
// the passes that make the later tables, the sky pass and scene_sky.glsl.
#ifndef ATMOSPHERE_TABLES_GLSL
#define ATMOSPHERE_TABLES_GLSL

#include "atmosphere.glsl"

// The light kept on the way to space, 0 below the horizon (the table holds only rays that leave).
vec3 transmittanceFromTable(sampler2D table, Atmosphere atmosphere, float radius, float cosZenith) {
    if (!aboveHorizon(atmosphere, radius, cosZenith)) {
        return vec3(0.0);
    }

    return textureLod(table, transmittanceLutUv(atmosphere, radius, cosZenith), 0.0).rgb;
}

vec3 multipleScatteringFromTable(sampler2D table, Atmosphere atmosphere, float radius, float cosSunZenith) {
    return textureLod(table, multipleScatteringLutUv(atmosphere, radius, cosSunZenith), 0.0).rgb;
}

// The sky's light along a world direction from the camera (without the discs): the sky lit by the sun plus the sky lit
// by the moon, each looked up by its own light's bearing.
vec3 skyFromTable(sampler2D table, Atmosphere atmosphere, vec3 direction) {
    vec3 sky = vec3(0.0);

    for (int light = 0; light < ATMOSPHERE_SKY_VIEW_LIGHTS; ++light) {
        SkyViewDirection lookup = skyViewLookup(atmosphere, atmosphere.radii.w, direction, atmosphereLightDirection(atmosphere, light));
        sky += textureLod(table, skyViewAtlasUv(skyViewLutUv(atmosphere, atmosphere.radii.w, lookup), light), 0.0).rgb;
    }

    return sky;
}

#endif
