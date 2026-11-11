#ifndef SKY_VISIBILITY_GLSL
#define SKY_VISIBILITY_GLSL

// The sky visibility grid's maths (render/gpu/sky_visibility.h, which explains each): the open sphere's two harmonic
// bands, the share of the sky's light a surface, a direction or the air gets from them, a grid point unpacked, and
// whether a point sees past its reach. Pure functions, mirrored by sky_visibility.cpp; the GPU tests check them
// against it. The grid's reads themselves live in scene_sky_visibility.glsl.

#include "render/gpu/sky_visibility_shared.h"

struct SkyVisibility {
    float open; // a: the share of the sphere open
    vec3 lean;  // b: towards the open side
};

struct SkyCell {
    SkyVisibility visibility;
    vec3 reachPositive; // how far it sees along +x, +y and +z, as shares of its cascade's spacing
    vec3 reachNegative; // along -x, -y and -z
    int state;          // SKY_VISIBILITY_UNKNOWN, _BAKED or _BURIED
};

float skyWithBounce(float seen, float blockersLit, float bounceShare) {
    return seen + bounceShare * blockersLit * (1.0 - seen);
}

float skySeenFacing(SkyVisibility visibility, vec3 normal) {
    return clamp(visibility.open + (2.0 / 3.0) * dot(visibility.lean, normal), 0.0, 1.0);
}

float skySeenAlong(SkyVisibility visibility, vec3 direction) {
    return clamp(visibility.open + dot(visibility.lean, direction), 0.0, 1.0);
}

float skyBlockersLit(SkyVisibility visibility, float floorShare) {
    return max(clamp(2.0 * visibility.open, 0.0, 1.0), floorShare);
}

float skyVisibilityFacing(SkyVisibility visibility, vec3 normal, float bounceShare, float blockersFloor) {
    return skyWithBounce(skySeenFacing(visibility, normal), skyBlockersLit(visibility, blockersFloor), bounceShare);
}

float skyVisibilityAlong(SkyVisibility visibility, vec3 direction, float bounceShare, float blockersFloor) {
    return skyWithBounce(skySeenAlong(visibility, direction), skyBlockersLit(visibility, blockersFloor), bounceShare);
}

float skyVisibilityInAir(SkyVisibility visibility, float bounceShare, float blockersFloor) {
    return skyWithBounce(clamp(visibility.open, 0.0, 1.0), skyBlockersLit(visibility, blockersFloor), bounceShare);
}

// GpuSkyCell's words: a and b.x as halves, b.y and b.z as halves, the reaches along +x -x +y -y in bytes, those along
// +z -z and the state in the top byte.
SkyCell unpackSkyCell(uvec4 words) {
    vec2 first = unpackHalf2x16(words.x);
    vec2 second = unpackHalf2x16(words.y);
    vec4 reaches = vec4((uvec4(words.z) >> uvec4(0u, 8u, 16u, 24u)) & 0xFFu) / float(SKY_VISIBILITY_REACH_STEPS);
    vec2 reachesZ = vec2((uvec2(words.w) >> uvec2(0u, 8u)) & 0xFFu) / float(SKY_VISIBILITY_REACH_STEPS);

    SkyCell cell;
    cell.visibility = SkyVisibility(first.x, vec3(first.y, second.x, second.y));
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
