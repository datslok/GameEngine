#include "render/gpu/virtual_shadows.h"

#include "math/mat4.h"
#include "render/gpu/cascades.h"

#include <cmath>
#include <stdexcept>

namespace {
    // The positive remainder, so pages left of the world's origin wrap like those right of it.
    int wrapped(std::int64_t value, int count) {
        const std::int64_t remainder = value % count;
        return static_cast<int>(remainder < 0 ? remainder + count : remainder);
    }

    // The camera across the light (x, y) and along it (z, metres along the light's direction), in doubles: the light's
    // rows taken as they are, the products and sums kept wide so a camera kilometres out still places its finest
    // level to a fraction of a texel.
    struct LightPlace {
        double x = 0.0;
        double y = 0.0;
        double along = 0.0;
    };

    LightPlace lightPlaceOf(const Mat4& rotation, const Vec3& point) {
        const auto row = [&](int index) {
            return static_cast<double>(rotation.values[index][0]) * point.x + static_cast<double>(rotation.values[index][1]) * point.y +
                   static_cast<double>(rotation.values[index][2]) * point.z;
        };

        // The view looks down -z, so a point's distance along the light is -z.
        return LightPlace{row(0), row(1), -row(2)};
    }

    bool sameVector(const Vec3& a, const Vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    // Each level's depth range reaches slack half-widths either side of its centre; points it shadows are at most a
    // half-width from the camera, so the centre can stay while the camera is within slack - 1 half-widths of it.
    bool depthRangeHolds(double centre, double cameraAlong, float halfWidth, float slack) {
        return std::abs(cameraAlong - centre) <= static_cast<double>(halfWidth) * (static_cast<double>(slack) - 1.0);
    }
}

int virtualShadowPagesPerSide(VirtualShadowResolution resolution) {
    return resolution == VirtualShadowResolution::Pages64 ? 64 : 32;
}

void visit(VirtualShadowSettings& settings, SettingsVisitor& visitor) {
    visitor.field("First level half-width", settings.firstHalfWidth, {.min = 0.05f, .max = 100.0f, .sliderMax = 4.0f, .unit = "m"});
    visitor.field("Levels", settings.levelCount, {.min = 1, .max = maxVirtualShadowLevels});
    visitor.choice("Level resolution", settings.resolution, virtualShadowResolutionNames);
    visitor.field("Depth slack", settings.depthSlack, {.min = 2.0f, .max = 1000.0f, .sliderMax = 64.0f, .unit = "half-widths"});
}

void validateVirtualShadows(const VirtualShadowSettings& settings) {
    if (!(settings.firstHalfWidth > 0.0f)) {
        throw std::invalid_argument("Virtual shadow maps need a first level wider than 0");
    }

    if (settings.levelCount < 1 || settings.levelCount > maxVirtualShadowLevels) {
        throw std::invalid_argument("Virtual shadow maps take 1 to VIRTUAL_SHADOW_MAX_LEVELS levels");
    }

    // Below 1 the range would not hold the level's own points; at 1 it would be re-centred at every step.
    if (!(settings.depthSlack > 1.0f)) {
        throw std::invalid_argument("Virtual shadow maps need a depth slack above 1");
    }
}

float virtualShadowHalfWidth(float firstHalfWidth, int level) {
    return std::ldexp(firstHalfWidth, level);
}

float virtualShadowPageWidth(float halfWidth, int pagesPerSide) {
    return 2.0f * halfWidth / static_cast<float>(pagesPerSide);
}

float virtualShadowTexelSize(float halfWidth, int pagesPerSide) {
    return virtualShadowPageWidth(halfWidth, pagesPerSide) / static_cast<float>(virtualShadowPageTexels);
}

float virtualShadowUsableHalfWidth(float halfWidth, int pagesPerSide) {
    return halfWidth - static_cast<float>(virtualShadowMarginPages) * virtualShadowPageWidth(halfWidth, pagesPerSide);
}

int virtualShadowLevelFor(float distance, float firstUsableHalfWidth, int levelCount) {
    float reach = firstUsableHalfWidth;

    for (int level = 0; level < levelCount; ++level) {
        if (distance <= reach) {
            return level;
        }

        reach *= 2.0f;
    }

    return virtualShadowNoLevel;
}

VirtualShadowPage virtualShadowPageOf(float texelX, float texelY) {
    const float page = static_cast<float>(virtualShadowPageTexels);
    return VirtualShadowPage{static_cast<int>(std::floor(texelX / page)), static_cast<int>(std::floor(texelY / page))};
}

VirtualShadowPage virtualShadowSlot(const VirtualShadowPage& pageInLevel, const VirtualShadowPage& originSlot, int pagesPerSide) {
    return VirtualShadowPage{(pageInLevel.x + originSlot.x) % pagesPerSide, (pageInLevel.y + originSlot.y) % pagesPerSide};
}

int virtualShadowTableIndex(int level, const VirtualShadowPage& slot, int pagesPerSide) {
    return (level * pagesPerSide + slot.y) * pagesPerSide + slot.x;
}

/*
* Each level is centred on the camera to the nearest whole page across the light, so the camera is at most half a page
* off its centre. Along the light the level keeps its depth centre while that still holds the camera's points (see
* depthRangeHolds), and is re-centred on the camera otherwise, dropping its pages; so is every level when the light,
* the settings or the caster reach changed, since every depth was drawn for those.
*/
VirtualShadowClipmapUpdate updateVirtualShadowClipmap(const std::optional<VirtualShadowClipmap>& previous, const Vec3& camera,
                                                      const Vec3& lightDirection, const VirtualShadowSettings& settings,
                                                      float casterReach) {
    validateVirtualShadows(settings);

    const bool sameSetUp = previous && sameVector(previous->lightDirection, lightDirection) && previous->settings == settings &&
                           previous->casterReach == casterReach;
    const int pagesPerSide = virtualShadowPagesPerSide(settings.resolution);
    const LightPlace place = lightPlaceOf(sunLightRotation(lightDirection), camera);

    VirtualShadowClipmapUpdate update;
    update.clipmap.lightDirection = lightDirection;
    update.clipmap.settings = settings;
    update.clipmap.casterReach = casterReach;

    for (int level = 0; level < settings.levelCount; ++level) {
        const float halfWidth = virtualShadowHalfWidth(settings.firstHalfWidth, level);
        const double pageWidth = static_cast<double>(virtualShadowPageWidth(halfWidth, pagesPerSide));
        VirtualShadowLevel& state = update.clipmap.levels[static_cast<std::size_t>(level)];

        state.firstPageX = std::llround(place.x / pageWidth) - pagesPerSide / 2;
        state.firstPageY = std::llround(place.y / pageWidth) - pagesPerSide / 2;
        state.depthCentre = place.along;

        if (sameSetUp) {
            const double previousCentre = previous->levels[static_cast<std::size_t>(level)].depthCentre;

            if (depthRangeHolds(previousCentre, place.along, halfWidth, settings.depthSlack)) {
                state.depthCentre = previousCentre;
                continue;
            }
        }

        update.droppedLevels |= 1u << level;
    }

    return update;
}

/*
* Measured from the camera: a point's texel is its light-space offset from the camera over the texel size, plus where
* the camera is in the level's texels (worked out in doubles from the level's first page, then small). Its depth runs
* from the sunward end of the range (the depth centre less slack half-widths and the caster reach: 1) to the far end
* (the centre plus slack half-widths: 0), the reversed-Z the renderer uses everywhere.
*/
VirtualShadowPlacement virtualShadowPlacement(const VirtualShadowClipmap& clipmap, int level, const Vec3& camera) {
    const VirtualShadowSettings& settings = clipmap.settings;
    const int pagesPerSide = virtualShadowPagesPerSide(settings.resolution);
    const float halfWidth = virtualShadowHalfWidth(settings.firstHalfWidth, level);
    const double pageWidth = static_cast<double>(virtualShadowPageWidth(halfWidth, pagesPerSide));
    const double texel = pageWidth / static_cast<double>(virtualShadowPageTexels);
    const VirtualShadowLevel& state = clipmap.levels[static_cast<std::size_t>(level)];
    const LightPlace place = lightPlaceOf(sunLightRotation(clipmap.lightDirection), camera);

    const double slackDepth = static_cast<double>(halfWidth) * static_cast<double>(settings.depthSlack);
    const double rangeDepth = 2.0 * slackDepth + static_cast<double>(clipmap.casterReach);
    const double farEnd = state.depthCentre + slackDepth;

    VirtualShadowPlacement placement;
    placement.cameraTexelX = static_cast<float>((place.x - static_cast<double>(state.firstPageX) * pageWidth) / texel);
    placement.cameraTexelY = static_cast<float>((place.y - static_cast<double>(state.firstPageY) * pageWidth) / texel);
    placement.inverseTexel = static_cast<float>(1.0 / texel);

    // A point's distance along the light is the camera's less its light-space z (the view looks down -z).
    placement.depthScale = static_cast<float>(1.0 / rangeDepth);
    placement.depthOffset = static_cast<float>((farEnd - place.along) / rangeDepth);
    placement.originSlot = VirtualShadowPage{wrapped(state.firstPageX, pagesPerSide), wrapped(state.firstPageY, pagesPerSide)};
    return placement;
}

Vec3 virtualShadowLevelPoint(const Vec3& lightPoint, const VirtualShadowPlacement& placement) {
    return Vec3{lightPoint.x * placement.inverseTexel + placement.cameraTexelX, lightPoint.y * placement.inverseTexel + placement.cameraTexelY,
                lightPoint.z * placement.depthScale + placement.depthOffset};
}
