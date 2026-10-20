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

// The sky's light along a world direction from the camera (without the sun's disc).
vec3 skyFromTable(sampler2D table, Atmosphere atmosphere, vec3 direction) {
    SkyViewDirection lookup = skyViewLookup(atmosphere, atmosphere.radii.w, direction, atmosphere.sun.xyz);
    return textureLod(table, skyViewLutUv(atmosphere, atmosphere.radii.w, lookup), 0.0).rgb;
}

#endif
