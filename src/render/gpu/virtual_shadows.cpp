#include "render/gpu/virtual_shadows.h"

#include "render/gpu/cascades.h"
#include "render/gpu/column_major.h"

#include <algorithm>
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
    visitor.field("Enabled", settings.enabled);
    visitor.field("Show pages", settings.showPages);
    visitor.field("Pool pages", settings.poolPages, {.min = 64, .max = maxVirtualShadowPoolPages});
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

    if (settings.poolPages < 1 || settings.poolPages > maxVirtualShadowPoolPages) {
        throw std::invalid_argument("Virtual shadow maps' pool takes 1 to VIRTUAL_SHADOW_MAX_POOL_PAGES pages");
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

VirtualShadowPageSpan virtualShadowMarkedPages(float texelX, float texelY, float reach, int pagesPerSide) {
    const VirtualShadowPage low = virtualShadowPageOf(texelX - reach, texelY - reach);
    const VirtualShadowPage high = virtualShadowPageOf(texelX + reach, texelY + reach);
    const auto clamped = [pagesPerSide](int page) { return std::clamp(page, 0, pagesPerSide - 1); };
    return VirtualShadowPageSpan{VirtualShadowPage{clamped(low.x), clamped(low.y)}, VirtualShadowPage{clamped(high.x), clamped(high.y)}};
}

VirtualShadowPage virtualShadowPageOfSlot(const VirtualShadowPage& slot, const VirtualShadowPage& originSlot, int pagesPerSide) {
    return VirtualShadowPage{(slot.x - originSlot.x + pagesPerSide) % pagesPerSide, (slot.y - originSlot.y + pagesPerSide) % pagesPerSide};
}

// Wrapping 32-bit sums: two pages of the world 2^32 pages apart would share a key, which no level spans.
VirtualShadowKey virtualShadowKeyOf(const VirtualShadowPage& pageInLevel, std::int32_t firstPageX, std::int32_t firstPageY, std::uint32_t generation) {
    return VirtualShadowKey{static_cast<std::uint32_t>(firstPageX) + static_cast<std::uint32_t>(pageInLevel.x),
                            static_cast<std::uint32_t>(firstPageY) + static_cast<std::uint32_t>(pageInLevel.y), generation};
}

bool virtualShadowEntryStale(std::uint32_t entry, const VirtualShadowKey& stored, const VirtualShadowKey& current) {
    return (entry & virtualShadowEntryAllocated) != 0 && !(stored == current);
}

bool virtualShadowEntryNeeds(std::uint32_t entry, bool stale) {
    return (entry & virtualShadowEntryRequested) != 0 && ((entry & virtualShadowEntryAllocated) == 0 || stale);
}

std::uint32_t virtualShadowPhysicalPage(std::uint32_t entry) {
    return entry & VIRTUAL_SHADOW_ENTRY_PHYSICAL;
}

VirtualShadowSource virtualShadowAllocationSource(std::uint32_t k, std::uint32_t freeTop, std::uint32_t cached) {
    if (k < freeTop) {
        return VirtualShadowSource{VIRTUAL_SHADOW_FROM_FREE, freeTop - 1u - k};
    }

    if (k - freeTop < cached) {
        return VirtualShadowSource{VIRTUAL_SHADOW_FROM_CACHED, k - freeTop};
    }

    return VirtualShadowSource{VIRTUAL_SHADOW_FROM_NOWHERE, 0};
}

std::uint32_t virtualShadowEntryWith(std::uint32_t physicalPage, std::uint32_t flags) {
    return (flags & VIRTUAL_SHADOW_ENTRY_FLAGS) | (physicalPage & VIRTUAL_SHADOW_ENTRY_PHYSICAL);
}

Vec3 virtualShadowLevelColour(int level) {
    static const std::array<Vec3, 6> colours{Vec3{0.2f, 0.4f, 1.0f}, Vec3{0.2f, 0.9f, 0.3f}, Vec3{1.0f, 0.9f, 0.2f},
                                          Vec3{0.2f, 0.9f, 0.9f}, Vec3{1.0f, 0.55f, 0.1f}, Vec3{0.65f, 0.3f, 1.0f}};
    return colours[static_cast<std::size_t>(level % 6)];
}

bool virtualShadowNearPageEdge(float texelX, float texelY, float width) {
    const float page = static_cast<float>(virtualShadowPageTexels);
    const auto near = [&](float texel) {
        const float within = texel - std::floor(texel / page) * page;
        return within < width || within > page - width;
    };
    return near(texelX) || near(texelY);
}

VirtualShadowCounts virtualShadowCountsFrom(const std::array<std::uint32_t, VIRTUAL_SHADOW_COUNTERS>& counters) {
    VirtualShadowCounts counts;
    counts.requested = counters[VIRTUAL_SHADOW_COUNT_REQUESTED];
    counts.kept = counters[VIRTUAL_SHADOW_COUNT_KEPT];
    counts.allocated = counters[VIRTUAL_SHADOW_COUNT_ALLOCATED];
    counts.evicted = counters[VIRTUAL_SHADOW_COUNT_EVICTED];
    counts.failed = counters[VIRTUAL_SHADOW_COUNT_FAILED];
    counts.stale = counters[VIRTUAL_SHADOW_COUNT_STALE];
    counts.free = counters[VIRTUAL_SHADOW_COUNT_FREE];
    return counts;
}

/*
* The levels past the clipmap's count are left zero; the shader reads only counts.x of them. The light's rotation is
* its orientation alone, so it turns camera-relative points into the light's axes as it turns world points.
*/
GpuVirtualShadowFrame packVirtualShadowFrame(const VirtualShadowClipmap& clipmap, const Vec3& camera,
                                             const std::array<std::uint32_t, maxVirtualShadowLevels>& generations,
                                             const Mat4& inverseViewProjection, float width, float height, bool showPages,
                                             std::uint32_t frame) {
    const VirtualShadowSettings& settings = clipmap.settings;
    const int pagesPerSide = virtualShadowPagesPerSide(settings.resolution);
    GpuVirtualShadowFrame packed{};
    writeColumnMajor(sunLightRotation(clipmap.lightDirection), packed.lightRotation);
    writeColumnMajor(inverseViewProjection, packed.inverseViewProjection);
    packed.screen[0] = width;
    packed.screen[1] = height;
    packed.screen[2] = width > 0.0f ? 1.0f / width : 0.0f;
    packed.screen[3] = height > 0.0f ? 1.0f / height : 0.0f;
    packed.options[0] = virtualShadowUsableHalfWidth(settings.firstHalfWidth, pagesPerSide);
    packed.options[1] = showPages ? 1.0f : 0.0f;
    packed.counts[0] = settings.levelCount;
    packed.counts[1] = pagesPerSide;
    packed.counts[2] = settings.poolPages;
    packed.counts[3] = static_cast<std::int32_t>(frame);

    for (int level = 0; level < settings.levelCount; ++level) {
        const VirtualShadowPlacement placement = virtualShadowPlacement(clipmap, level, camera);
        const VirtualShadowLevel& state = clipmap.levels[static_cast<std::size_t>(level)];
        GpuVirtualShadowLevel& out = packed.levels[level];
        out.placement[0] = placement.cameraTexelX;
        out.placement[1] = placement.cameraTexelY;
        out.placement[2] = placement.inverseTexel;
        out.placement[3] = placement.depthScale;
        out.depth[0] = placement.depthOffset;
        out.pages[0] = placement.originSlot.x;
        out.pages[1] = placement.originSlot.y;
        out.pages[2] = static_cast<std::int32_t>(static_cast<std::uint32_t>(state.firstPageX & 0xFFFFFFFF));
        out.pages[3] = static_cast<std::int32_t>(static_cast<std::uint32_t>(state.firstPageY & 0xFFFFFFFF));
        out.key[0] = generations[static_cast<std::size_t>(level)];
    }

    return packed;
}
