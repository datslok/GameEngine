#ifndef VIRTUAL_SHADOW_RAYS_GLSL
#define VIRTUAL_SHADOW_RAYS_GLSL

// Soft shadows through the sun's virtual shadow pages by shadow-map ray tracing (ROADMAP phase 21 4d-i): rays over the
// sun's disc marched through a level's depths, surfaces as thin shells, a hidden one carried on behind a higher one.
// Mirrors render/gpu/virtual_shadow_rays.h, which explains it; the main pass's march is virtualShadowSun
// (scene_surface.glsl).

#include "render/gpu/virtual_shadows_shared.h"
#include "virtual_shadows.glsl"

const float virtualShadowGoldenFraction = 0.6180339887;

// A ray's point on the sun's unit disc: a Fibonacci (Vogel) spiral turned by turn (whole turns).
vec2 virtualShadowRayDisc(int ray, int rays, float turn) {
    float radius = sqrt((float(ray) + 0.5) / float(rays));
    float angle = 6.28318530718 * fract(float(ray) * virtualShadowGoldenFraction + turn);
    return radius * vec2(cos(angle), sin(angle));
}

// How far up its ray a sample lies, a share of the ray's height: the last at its end, the rest by squares.
float virtualShadowSampleShare(int sampleIndex, int samples, float jitter) {
    if (sampleIndex >= samples - 1) {
        return 1.0;
    }

    float share = (float(sampleIndex) + jitter) / float(samples);
    return share * share;
}

// A ray's height (metres): its rim reaching VIRTUAL_SHADOW_RAY_TEXELS across, no higher than the level's range.
float virtualShadowRayHeight(float tanAngularRadius, float inverseTexel, float depthScale, float startDepth) {
    float toRangeEnd = max(1.0 - startDepth, 0.0) / depthScale;
    float acrossPerMetre = tanAngularRadius * inverseTexel;
    return acrossPerMetre > 0.0 ? min(float(VIRTUAL_SHADOW_RAY_TEXELS) / acrossPerMetre, toRangeEnd) : toRangeEnd;
}

// A sample height metres up a ray towards disc point disc, from start (texel xy, biased depth z).
vec3 virtualShadowRaySample(vec3 start, vec2 disc, float tanAngularRadius, float inverseTexel, float depthScale, float height) {
    return vec3(start.xy + disc * (height * tanAngularRadius * inverseTexel), start.z + height * depthScale);
}

// The first of the at most 2 x 2 pages a point's samples land on, and which of them a sample's page is (0 to 3).
ivec2 virtualShadowFootprintFirst(vec2 texel) {
    return virtualShadowPageOf(texel - float(VIRTUAL_SHADOW_RAY_TEXELS));
}

int virtualShadowFootprintIndex(ivec2 page, ivec2 first) {
    return (page.x - first.x) + 2 * (page.y - first.y);
}

// A ray's march so far (render/gpu/virtual_shadow_rays.h, VirtualShadowMarch).
struct VirtualShadowMarch {
    bool under;
    bool hit;
    float surfaceDepth;
    float surfaceRayDepth;
    float surfaceSlope;
};

VirtualShadowMarch virtualShadowMarchStart() {
    return VirtualShadowMarch(false, false, 0.0, 0.0, 0.0);
}

VirtualShadowMarch virtualShadowOnSurface(VirtualShadowMarch march, float stored, float rayDepth, float slope) {
    march.under = true;
    march.surfaceDepth = stored;
    march.surfaceRayDepth = rayDepth;
    march.surfaceSlope = slope;
    return march;
}

// One sample of the march (virtualShadowMarchStep's rules: under, the same surface on, a hidden one carried on, out).
VirtualShadowMarch virtualShadowMarchStep(VirtualShadowMarch march, float rayDepth, float stored, float tolerance, float slopeCap) {
    if (march.hit) {
        return march;
    }

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
            return march;
        }

        return virtualShadowOnSurface(march, stored, rayDepth, 0.0);
    }

    if (march.under) {
        march.hit = rayDepth + tolerance >= carried;
        march.under = false;
    }

    return march;
}

// After the last sample: blocked if it hit, or if it ends still under a surface.
bool virtualShadowRayBlocked(VirtualShadowMarch march) {
    return march.hit || march.under;
}

#endif
