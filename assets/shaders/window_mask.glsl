// Slit windows on walls without geometry, included with #include "window_mask.glsl": mirrors render/gpu/window_mask.cpp,
// which tests it (and explains it); tests/gpu checks it against it on the GPU. Pure functions: the band strip is read
// in the main pass (scene_surface.glsl).

#ifndef WINDOW_MASK_GLSL
#define WINDOW_MASK_GLSL

#include "hash.glsl"
#include "render/gpu/window_mask_shared.h"

// How much of [0, x] periodic slits cover: one, length long, in the middle of each pitch-wide cell (pulseIntegral).
float pulseIntegral(float x, float pitch, float length) {
    float cell = floor(x / pitch);
    float inCell = x - cell * pitch;
    float start = 0.5 * (pitch - length);
    return cell * length + clamp(inCell - start, 0.0, length);
}

// The slits' share of a pixel's footprint round centre, box-filtered (pulseCoverage); no footprint samples the point.
float pulseCoverage(float centre, float footprint, float pitch, float length) {
    if (footprint <= 1e-4 * pitch) {
        float inCell = centre - floor(centre / pitch) * pitch;
        float start = 0.5 * (pitch - length);
        return inCell >= start && inCell < start + length ? 1.0 : 0.0;
    }

    return (pulseIntegral(centre + 0.5 * footprint, pitch, length) - pulseIntegral(centre - 0.5 * footprint, pitch, length)) / footprint;
}

// A face's seed from its facet numbers, each rounded to WINDOW_SEED_STEPS (windowFaceSeed).
uint windowFaceSeed(vec2 facetNumbers) {
    uint x = uint(floor(facetNumbers.x * float(WINDOW_SEED_STEPS) + 0.5));
    uint y = uint(floor(facetNumbers.y * float(WINDOW_SEED_STEPS) + 0.5));
    return x | (y << 11u);
}

// A window cell's three random numbers 0..1 from its column, row and face (windowHash).
vec3 windowHash(float across, float height, float pitch, uint faceSeed) {
    uint column = uint(int(floor(across / pitch)));
    uint row = uint(int(floor(height / float(WINDOW_ROW_HEIGHT))));
    uvec3 hashed = pcg3d(uvec3(column, row, faceSeed));
    return vec3(hashed >> 8u) * (1.0 / 16777216.0);
}

// How far the windows have faded to their average, from the pixel's footprint in cells (windowFarShare).
float windowFarShare(vec2 footprint, float pitch) {
    float cells = max(footprint.x / pitch, footprint.y / float(WINDOW_ROW_HEIGHT));
    return smoothstep(float(WINDOW_FAR_START), float(WINDOW_FAR_FULL), cells);
}

// The light a wall's pixel gives off (windowEmission): band x rows' cover, y share lit, z share cool, w slit's share across.
vec3 windowEmission(vec2 place, vec2 footprint, vec4 band, float pitch, uint faceSeed, vec3 warm, vec3 cool) {
    if (!(band.x > 0.0) || !(band.w > 0.0) || !(pitch > 0.0)) {
        return vec3(0.0);
    }

    float across = pulseCoverage(place.x, footprint.x, pitch, band.w * pitch);
    vec3 numbers = windowHash(place.x, place.y, pitch, faceSeed);
    float far = windowFarShare(footprint, pitch);
    float lit = (numbers.x < band.y ? 1.0 : 0.0) * (1.0 - far) + band.y * far;
    vec3 own = numbers.y < band.z ? cool : warm;
    vec3 colour = mix(own, mix(warm, cool, band.z), far);
    float brightness = (float(WINDOW_LEAST_BRIGHTNESS) + float(WINDOW_BRIGHTNESS_RANGE) * numbers.z) * (1.0 - far) + far;
    return colour * (band.x * across * lit * brightness);
}

#endif
