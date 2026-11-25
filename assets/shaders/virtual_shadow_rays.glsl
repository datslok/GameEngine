#ifndef VIRTUAL_SHADOW_RAYS_GLSL
#define VIRTUAL_SHADOW_RAYS_GLSL

// Soft shadows through the sun's virtual shadow pages by shadow-map ray tracing (ROADMAP phase 21 4d): rays over the
// sun's disc marched through a point's level and coarser ones, surfaces as thin shells, a hidden one carried on behind
// a higher one, a crossing between two samples refined by one more read. Mirrors render/gpu/virtual_shadow_rays.h,
// which explains it; the main pass's march is virtualShadowRayShadow (scene_surface.glsl).

#include "render/gpu/virtual_shadows_shared.h"
#include "virtual_shadows.glsl"

const float virtualShadowGoldenFraction = 0.6180339887;

// A ray's point on the sun's unit disc: a Fibonacci (Vogel) spiral turned by turn (whole turns).
vec2 virtualShadowRayDisc(int ray, int rays, float turn) {
    float radius = sqrt((float(ray) + 0.5) / float(rays));
    float angle = 6.28318530718 * fract(float(ray) * virtualShadowGoldenFraction + turn);
    return radius * vec2(cos(angle), sin(angle));
}

// How far up its ray a sample lies, a share of the ray's height: the last at its end, the rest by the power
// max(2, levelsUp) of (sample + jitter) / samples, by multiplying.
float virtualShadowSampleShare(int sampleIndex, int samples, float jitter, int levelsUp) {
    if (sampleIndex >= samples - 1) {
        return 1.0;
    }

    float share = (float(sampleIndex) + jitter) / float(samples);
    float powered = share * share;

    for (int power = 2; power < levelsUp; ++power) {
        powered *= share;
    }

    return powered;
}

// A ray's height (metres): its rim reaching VIRTUAL_SHADOW_RAY_TEXELS of the level levelsUp coarser, no higher than
// this level's range.
float virtualShadowRayHeight(float tanAngularRadius, float inverseTexel, float depthScale, float startDepth, int levelsUp) {
    float toRangeEnd = max(1.0 - startDepth, 0.0) / depthScale;
    float acrossPerMetre = tanAngularRadius * inverseTexel;
    float reach = ldexp(float(VIRTUAL_SHADOW_RAY_TEXELS), levelsUp);
    return acrossPerMetre > 0.0 ? min(reach / acrossPerMetre, toRangeEnd) : toRangeEnd;
}

// How many levels coarser a sample offset this many of the point's level's texels reads (by doubling, at most levelsUp).
int virtualShadowSampleLevel(float offsetTexels, int levelsUp) {
    float reach = float(VIRTUAL_SHADOW_RAY_TEXELS);
    int level = 0;

    while (level < levelsUp && offsetTexels > reach) {
        reach *= 2.0;
        ++level;
    }

    return level;
}

// A level's stored depth as a height along the light above the point (metres).
float virtualShadowStoredHeight(float stored, float pointDepth, float depthScale) {
    return (stored - pointDepth) / depthScale;
}

// A sample height metres up a ray towards disc point disc, from start (texel xy, depth z).
vec3 virtualShadowRaySample(vec3 start, vec2 disc, float tanAngularRadius, float inverseTexel, float depthScale, float height) {
    return vec3(start.xy + disc * (height * tanAngularRadius * inverseTexel), start.z + height * depthScale);
}

// The first of the at most 2 x 2 pages a point's samples land on in its own level, and which of them a sample's page is.
ivec2 virtualShadowFootprintFirst(vec2 texel) {
    return virtualShadowPageOf(texel - float(VIRTUAL_SHADOW_RAY_TEXELS));
}

int virtualShadowFootprintIndex(ivec2 page, ivec2 first) {
    return (page.x - first.x) + 2 * (page.y - first.y);
}

// A ray's march so far, in heights above the point (render/gpu/virtual_shadow_rays.h, VirtualShadowMarch).
struct VirtualShadowMarch {
    bool under;
    bool hit;
    float surfaceDepth;
    float surfaceRayDepth;
    float surfaceSlope;
    float lastRayDepth;
    bool checking;
    float checkDepth;
};

VirtualShadowMarch virtualShadowMarchStart() {
    return VirtualShadowMarch(false, false, 0.0, 0.0, 0.0, 0.0, false, 0.0);
}

VirtualShadowMarch virtualShadowOnSurface(VirtualShadowMarch march, float stored, float rayDepth, float slope) {
    march.under = true;
    march.lastRayDepth = rayDepth;
    march.surfaceDepth = stored;
    march.surfaceRayDepth = rayDepth;
    march.surfaceSlope = slope;
    return march;
}

// One sample of the march (virtualShadowMarchStep's rules: under, the same surface on, a hidden one carried on, out, a
// crossing to read).
VirtualShadowMarch virtualShadowMarchStep(VirtualShadowMarch march, float rayDepth, float stored, float tolerance, float slopeCap) {
    if (march.hit) {
        return march;
    }

    march.checking = false;
    float risen = rayDepth - march.surfaceRayDepth;
    float carried = march.surfaceDepth + march.surfaceSlope * risen;

    if (stored > rayDepth) {
        if (!march.under) {
            return virtualShadowOnSurface(march, stored, rayDepth, 0.0);
        }

        if (abs(stored - carried) <= tolerance + slopeCap * risen) {
            float slope = risen > 0.0 ? clamp((stored - march.surfaceDepth) / risen, -slopeCap, slopeCap) : march.surfaceSlope;
            return virtualShadowOnSurface(march, stored, rayDepth, slope);
        }

        if (stored > carried && slopeCap > 0.0) {
            march.hit = rayDepth + tolerance >= carried;
            march.lastRayDepth = rayDepth;
            return march;
        }

        return virtualShadowOnSurface(march, stored, rayDepth, 0.0);
    }

    if (march.under && rayDepth + tolerance >= carried) {
        march.checking = true;
        march.checkDepth = carried;
    } else if (!march.under && stored > march.lastRayDepth + tolerance) {
        march.checking = true;
        march.checkDepth = stored;
    }

    march.under = false;
    march.lastRayDepth = rayDepth;
    return march;
}

// The refinement read: where the ray crossed the asked height, and blocked if the surface reaches it there (or, found
// is false, no page holds it).
float virtualShadowCrossingDepth(VirtualShadowMarch march, float previousRayDepth, float rayDepth) {
    return clamp(march.checkDepth, previousRayDepth, rayDepth);
}

VirtualShadowMarch virtualShadowMarchCheck(VirtualShadowMarch march, bool found, float storedAtCrossing, float tolerance) {
    march.hit = !found || storedAtCrossing + tolerance >= march.checkDepth;
    march.checking = false;
    return march;
}

// After the last sample: blocked if it hit, or if it ends still under a surface.
bool virtualShadowRayBlocked(VirtualShadowMarch march) {
    return march.hit || march.under;
}

#endif
