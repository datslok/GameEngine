#ifndef SKY_VISIBILITY_GLSL
#define SKY_VISIBILITY_GLSL

// The sky visibility grid's maths (render/gpu/sky_visibility.h, which explains each): the open sphere's two harmonic
// bands, the light from all around a surface, a direction or the air gets from them and from the bounce light round
// the point, a grid point unpacked, and whether a point sees past its reach. Pure functions, mirrored by
// sky_visibility.cpp; the GPU tests check them against it. The grid's reads themselves live in scene_sky_visibility.glsl.

#include "render/gpu/sky_visibility_shared.h"

struct SkyVisibility {
    float open; // a: the share of the sphere open (for a bounce channel, the mean light brought back)
    vec3 lean;  // b: towards the open side (where the light comes from)
};

// The bounce light round a point per unit of the open sky's light: red, green and blue's bands (SkyBounce).
struct SkyBounce {
    SkyVisibility red;
    SkyVisibility green;
    SkyVisibility blue;
};

struct SkyCell {
    SkyVisibility visibility;
    SkyBounce bounce;
    SkyBounce sunBounce; // the sun's bounce, per unit of its irradiance
    vec3 reachPositive; // how far it sees along +x, +y and +z, as shares of its cascade's spacing
    vec3 reachNegative; // along -x, -y and -z
    int state;          // SKY_VISIBILITY_UNKNOWN, _BAKED or _BURIED
};

float skySeenFacing(SkyVisibility visibility, vec3 normal) {
    return clamp(visibility.open + (2.0 / 3.0) * dot(visibility.lean, normal), 0.0, 1.0);
}

float skySeenAlong(SkyVisibility visibility, vec3 direction) {
    return clamp(visibility.open + dot(visibility.lean, direction), 0.0, 1.0);
}

vec3 skyBounceFacing(SkyBounce bounce, vec3 normal) {
    return vec3(skySeenFacing(bounce.red, normal), skySeenFacing(bounce.green, normal), skySeenFacing(bounce.blue, normal));
}

vec3 skyBounceAlong(SkyBounce bounce, vec3 direction) {
    return vec3(skySeenAlong(bounce.red, direction), skySeenAlong(bounce.green, direction), skySeenAlong(bounce.blue, direction));
}

vec3 skyLightFacing(SkyVisibility visibility, SkyBounce bounce, vec3 normal, float floorShare) {
    return max(skySeenFacing(visibility, normal) + skyBounceFacing(bounce, normal), vec3(floorShare));
}

vec3 skyLightAlong(SkyVisibility visibility, SkyBounce bounce, vec3 direction, float floorShare) {
    return max(skySeenAlong(visibility, direction) + skyBounceAlong(bounce, direction), vec3(floorShare));
}

vec3 skyLightInAir(SkyVisibility visibility, SkyBounce bounce, float floorShare) {
    vec3 bounced = clamp(vec3(bounce.red.open, bounce.green.open, bounce.blue.open), 0.0, 1.0);
    return max(clamp(visibility.open, 0.0, 1.0) + bounced, vec3(floorShare));
}

// Two words of halves: a and b.x, then b.y and b.z.
SkyVisibility unpackSkyBands(uint first, uint second) {
    vec2 low = unpackHalf2x16(first);
    vec2 high = unpackHalf2x16(second);
    return SkyVisibility(low.x, vec3(low.y, high.x, high.y));
}

// GpuSkyCell's four texels: a and b as halves, the reaches along +x -x +y -y in bytes, those along +z -z and the state
// in the top byte; then the bounce's red, green and blue bands as halves, two words each; then the sun's bounce's.
SkyCell unpackSkyCell(uvec4 words, uvec4 bounceWords, uvec4 lastWords, uvec4 sunWords) {
    vec4 reaches = vec4((uvec4(words.z) >> uvec4(0u, 8u, 16u, 24u)) & 0xFFu) / float(SKY_VISIBILITY_REACH_STEPS);
    vec2 reachesZ = vec2((uvec2(words.w) >> uvec2(0u, 8u)) & 0xFFu) / float(SKY_VISIBILITY_REACH_STEPS);

    SkyCell cell;
    cell.visibility = unpackSkyBands(words.x, words.y);
    cell.bounce = SkyBounce(unpackSkyBands(bounceWords.x, bounceWords.y), unpackSkyBands(bounceWords.z, bounceWords.w),
                            unpackSkyBands(lastWords.x, lastWords.y));
    cell.sunBounce = SkyBounce(unpackSkyBands(lastWords.z, lastWords.w), unpackSkyBands(sunWords.x, sunWords.y), unpackSkyBands(sunWords.z, sunWords.w));
    cell.reachPositive = vec3(reaches.x, reaches.z, reachesZ.x);
    cell.reachNegative = vec3(reaches.y, reaches.w, reachesZ.y);
    cell.state = int(words.w >> 24u);
    return cell;
}

bool skyCornerSees(vec3 reachPositive, vec3 reachNegative, vec3 fromCorner, float spacing) {
    vec3 along = fromCorner / spacing;
    vec3 allowed = mix(reachPositive, reachNegative, lessThan(along, vec3(0.0)));
    return all(lessThanEqual(abs(along), allowed + 1.0 / float(SKY_VISIBILITY_REACH_STEPS)));
}

#endif
