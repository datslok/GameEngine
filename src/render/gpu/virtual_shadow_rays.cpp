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
        march.lastRayDepth = rayDepth;
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

// The power by multiplying, not pow, so the CPU and the GPU agree to rounding.
float virtualShadowSampleShare(int sample, int samples, float jitter, int levelsUp) {
    if (sample >= samples - 1) {
        return 1.0f;
    }

    const float share = (static_cast<float>(sample) + jitter) / static_cast<float>(samples);
    float powered = share * share;

    for (int power = 2; power < levelsUp; ++power) {
        powered *= share;
    }

    return powered;
}

// At the rim the ray moves tan(a) a metre up, which is tan(a) * inverseTexel texels: the reach's texels over that.
float virtualShadowRayHeight(float tanAngularRadius, float inverseTexel, float depthScale, float startDepth, int levelsUp) {
    const float toRangeEnd = std::max(1.0f - startDepth, 0.0f) / depthScale;
    const float acrossPerMetre = tanAngularRadius * inverseTexel;
    const float reach = std::ldexp(virtualShadowRayTexels, levelsUp);
    return acrossPerMetre > 0.0f ? std::min(reach / acrossPerMetre, toRangeEnd) : toRangeEnd;
}

int virtualShadowSampleLevel(float offsetTexels, int levelsUp) {
    float reach = virtualShadowRayTexels;
    int level = 0;

    while (level < levelsUp && offsetTexels > reach) {
        reach *= 2.0f;
        ++level;
    }

    return level;
}

float virtualShadowStoredHeight(float stored, float pointDepth, float depthScale) {
    return (stored - pointDepth) / depthScale;
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
    next.checking = false;
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
            next.lastRayDepth = rayDepth;
            return next;
        }

        return onSurface(next, stored, rayDepth, 0.0f);
    }

    if (march.under && rayDepth + tolerance >= carried) {
        // Out from under, risen to the surface's height: through it, or out beside it first? The crossing tells.
        next.checking = true;
        next.checkDepth = carried;
    } else if (!march.under && stored > march.lastRayDepth + tolerance) {
        // Risen past a surface the ray was below at the last sample: through it, or past its edge? The crossing tells.
        next.checking = true;
        next.checkDepth = stored;
    }

    next.under = false;
    next.lastRayDepth = rayDepth;
    return next;
}

float virtualShadowCrossingDepth(const VirtualShadowMarch& march, float previousRayDepth, float rayDepth) {
    return std::clamp(march.checkDepth, previousRayDepth, rayDepth);
}

VirtualShadowMarch virtualShadowMarchCheck(const VirtualShadowMarch& march, std::optional<float> storedAtCrossing, float tolerance) {
    VirtualShadowMarch next = march;
    next.hit = !storedAtCrossing.has_value() || *storedAtCrossing + tolerance >= march.checkDepth;
    next.checking = false;
    return next;
}

bool virtualShadowRayBlocked(const VirtualShadowMarch& march) {
    return march.hit || march.under;
}

/*
* Each sample's level by its spread across the point's own level's texels; its depth there turned into a height above
* the point, the ray's own height starting at the point's level's bias, the tolerance the sample's level's bias.
*/
float virtualShadowRayVisibility(const std::vector<VirtualShadowRayLevel>& levels, float tanAngularRadius, const VirtualShadowSettings& settings,
                                 float turn, float jitter, const std::function<std::optional<float>(int, float, float)>& storedAt) {
    const VirtualShadowRayLevel& own = levels.front();

    if (settings.rays <= 0) {
        const std::optional<float> stored = storedAt(0, own.point.x, own.point.y);
        return stored.has_value() ? virtualShadowLit(*stored, own.point.z, own.biasDepth) : 1.0f;
    }

    const int levelsUp = std::min(settings.rayLevels, static_cast<int>(levels.size()) - 1);
    const float startHeight = own.biasDepth / own.depthScale;
    const float height = virtualShadowRayHeight(tanAngularRadius, own.inverseTexel, own.depthScale, own.point.z + own.biasDepth, levelsUp);
    const float slopeCap = settings.extrapolateSlope * tanAngularRadius;
    int blocked = 0;

    for (int ray = 0; ray < settings.rays; ++ray) {
        const Vec2 disc = virtualShadowRayDisc(ray, settings.rays, turn);
        const float discReach = std::sqrt(disc.x * disc.x + disc.y * disc.y);
        const float rayJitter = fraction(jitter + static_cast<float>(ray) * goldenFraction);

        // The surface's height (and its level's tolerance) up the ray, from the first level holding it.
        const auto surfaceAt = [&](float up, float& tolerance) -> std::optional<float> {
            const int first = virtualShadowSampleLevel(discReach * up * tanAngularRadius * own.inverseTexel, levelsUp);

            for (int level = first; level < static_cast<int>(levels.size()); ++level) {
                const VirtualShadowRayLevel& in = levels[static_cast<std::size_t>(level)];
                const Vec3 at = virtualShadowRaySample(in.point, disc, tanAngularRadius, in.inverseTexel, in.depthScale, up);

                if (const std::optional<float> stored = storedAt(level, at.x, at.y)) {
                    tolerance = in.biasDepth / in.depthScale;
                    return virtualShadowStoredHeight(*stored, in.point.z, in.depthScale);
                }
            }

            return std::nullopt;
        };

        VirtualShadowMarch march;
        float previous = 0.0f;

        for (int sample = 0; sample < settings.samplesPerRay && !march.hit; ++sample) {
            const float up = height * virtualShadowSampleShare(sample, settings.samplesPerRay, rayJitter, levelsUp);
            float tolerance = 0.0f;

            if (const std::optional<float> surface = surfaceAt(up, tolerance)) {
                march = virtualShadowMarchStep(march, startHeight + up, *surface, tolerance, slopeCap);

                if (march.checking) {
                    const float crossing = virtualShadowCrossingDepth(march, startHeight + previous, startHeight + up) - startHeight;
                    float crossingTolerance = tolerance;
                    const std::optional<float> there = surfaceAt(crossing, crossingTolerance);
                    march = virtualShadowMarchCheck(march, there, crossingTolerance);
                }
            }

            previous = up;
        }

        blocked += virtualShadowRayBlocked(march) ? 1 : 0;
    }

    return 1.0f - static_cast<float>(blocked) / static_cast<float>(settings.rays);
}
