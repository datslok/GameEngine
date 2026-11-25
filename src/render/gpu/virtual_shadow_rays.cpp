#include "render/gpu/virtual_shadow_rays.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
    // The golden ratio's fraction: each ray (and each ray's jitter) a golden turn on from the last.
    constexpr float goldenFraction = 0.6180339887f;

    float fraction(float value) {
        return value - std::floor(value);
    }

    VirtualShadowMarch onSurface(VirtualShadowMarch march, float stored, float rayDepth, float slope) {
        march.under = true;
        march.surfaceDepth = stored;
        march.surfaceRayDepth = rayDepth;
        march.surfaceSlope = slope;
        return march;
    }
}

// Radius sqrt((i + 0.5) / n) gives every ray an equal ring of area; the golden angle spreads them round.
Vec2 virtualShadowRayDisc(int ray, int rays, float turn) {
    const float radius = std::sqrt((static_cast<float>(ray) + 0.5f) / static_cast<float>(rays));
    const float angle = 2.0f * std::numbers::pi_v<float> * fraction(static_cast<float>(ray) * goldenFraction + turn);
    return Vec2{radius * std::cos(angle), radius * std::sin(angle)};
}

float virtualShadowSampleShare(int sample, int samples, float jitter) {
    if (sample >= samples - 1) {
        return 1.0f;
    }

    const float share = (static_cast<float>(sample) + jitter) / static_cast<float>(samples);
    return share * share;
}

// At the rim the ray moves tan(a) a metre up, which is tan(a) * inverseTexel texels: the reach's texels over that.
float virtualShadowRayHeight(float tanAngularRadius, float inverseTexel, float depthScale, float startDepth) {
    const float toRangeEnd = std::max(1.0f - startDepth, 0.0f) / depthScale;
    const float acrossPerMetre = tanAngularRadius * inverseTexel;
    return acrossPerMetre > 0.0f ? std::min(virtualShadowRayTexels / acrossPerMetre, toRangeEnd) : toRangeEnd;
}

Vec3 virtualShadowRaySample(const Vec3& start, const Vec2& disc, float tanAngularRadius, float inverseTexel, float depthScale, float height) {
    const float across = height * tanAngularRadius * inverseTexel;
    return Vec3{start.x + disc.x * across, start.y + disc.y * across, start.z + height * depthScale};
}

VirtualShadowPage virtualShadowFootprintFirst(float texelX, float texelY) {
    return virtualShadowPageOf(texelX - virtualShadowRayTexels, texelY - virtualShadowRayTexels);
}

int virtualShadowFootprintIndex(const VirtualShadowPage& page, const VirtualShadowPage& first) {
    return (page.x - first.x) + 2 * (page.y - first.y);
}

VirtualShadowMarch virtualShadowMarchStep(const VirtualShadowMarch& march, float rayDepth, float stored, float tolerance, float slopeCap) {
    if (march.hit) {
        return march;
    }

    VirtualShadowMarch next = march;
    const float risen = rayDepth - march.surfaceRayDepth;
    const float carried = march.surfaceDepth + march.surfaceSlope * risen;

    if (stored > rayDepth) {
        if (!march.under) {
            return onSurface(next, stored, rayDepth, 0.0f);
        }

        if (std::abs(stored - carried) <= tolerance + slopeCap * risen) {
            const float slope = risen > 0.0f ? std::clamp((stored - march.surfaceDepth) / risen, -slopeCap, slopeCap) : march.surfaceSlope;
            return onSurface(next, stored, rayDepth, slope);
        }

        if (stored > carried && slopeCap > 0.0f) {
            // A higher surface hides the one the ray is under: that one carried on behind it, and met when risen to.
            next.hit = rayDepth + tolerance >= carried;
            return next;
        }

        return onSurface(next, stored, rayDepth, 0.0f);
    }

    if (march.under) {
        next.hit = rayDepth + tolerance >= carried;
        next.under = false;
    }

    return next;
}

bool virtualShadowRayBlocked(const VirtualShadowMarch& march) {
    return march.hit || march.under;
}

float virtualShadowRayVisibility(const Vec3& start, float biasDepth, float tanAngularRadius, float inverseTexel, float depthScale,
                                 const VirtualShadowSettings& settings, float turn, float jitter,
                                 const std::function<std::optional<float>(float, float)>& storedAt) {
    const Vec3 lifted{start.x, start.y, start.z + biasDepth};

    if (settings.rays <= 0) {
        const std::optional<float> stored = storedAt(start.x, start.y);
        return stored.has_value() ? virtualShadowLit(*stored, start.z, biasDepth) : 1.0f;
    }

    const float height = virtualShadowRayHeight(tanAngularRadius, inverseTexel, depthScale, lifted.z);
    const float slopeCap = settings.extrapolateSlope * tanAngularRadius;
    int blocked = 0;

    for (int ray = 0; ray < settings.rays; ++ray) {
        const Vec2 disc = virtualShadowRayDisc(ray, settings.rays, turn);
        const float rayJitter = fraction(jitter + static_cast<float>(ray) * goldenFraction);
        VirtualShadowMarch march;

        for (int sample = 0; sample < settings.samplesPerRay && !march.hit; ++sample) {
            const float up = height * virtualShadowSampleShare(sample, settings.samplesPerRay, rayJitter);
            const Vec3 point = virtualShadowRaySample(lifted, disc, tanAngularRadius, inverseTexel, depthScale, up);
            const std::optional<float> stored = storedAt(point.x, point.y);

            if (stored.has_value()) {
                march = virtualShadowMarchStep(march, point.z, *stored, biasDepth, slopeCap);
            }
        }

        blocked += virtualShadowRayBlocked(march) ? 1 : 0;
    }

    return 1.0f - static_cast<float>(blocked) / static_cast<float>(settings.rays);
}
