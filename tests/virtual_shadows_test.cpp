#include "engine/debug_views.h"
#include "engine/settings_file.h"
#include "math/frustum.h"
#include "math/mat4.h"
#include "render/gpu/cascades.h"
#include "render/gpu/virtual_shadows.h"
#include "render/render_stats.h"
#include "test_levels.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {
    constexpr float casterReach = 200.0f;

    Vec3 randomDirection(std::mt19937& random) {
        std::normal_distribution<float> normal(0.0f, 1.0f);
        Vec3 direction;

        do {
            direction = Vec3{normal(random), normal(random), normal(random)};
        } while (direction.lengthSquared() < 1e-4f);

        // The sun shines downwards.
        direction = direction.normalized();
        return direction.y > 0.0f ? Vec3{direction.x, -direction.y, direction.z} : direction;
    }

    Vec3 lightPointOf(const Vec3& lightDirection, const Vec3& relative) {
        const Vec4 rotated = sunLightRotation(lightDirection) * Vec4{relative.x, relative.y, relative.z, 0.0f};
        return Vec3{rotated.x, rotated.y, rotated.z};
    }

    // A point's texel and depth in a level, measured from the camera as the shader will.
    Vec3 inLevel(const VirtualShadowClipmap& clipmap, int level, const Vec3& camera, const Vec3& point) {
        return virtualShadowLevelPoint(lightPointOf(clipmap.lightDirection, point - camera), virtualShadowPlacement(clipmap, level, camera));
    }

    VirtualShadowClipmap freshClipmap(const Vec3& camera, const Vec3& lightDirection, const VirtualShadowSettings& settings = {}) {
        return updateVirtualShadowClipmap(std::nullopt, camera, lightDirection, settings, casterReach).clipmap;
    }

    // A level's texel and depth (0 its range's far end, 1 its sunward end with the caster reach) back in the world: the
    // placement undone, then the light's rotation (its transpose) turned back.
    Vec3 levelPointInWorld(const VirtualShadowClipmap& clipmap, int level, const Vec3& camera, const Vec3& texelAndDepth) {
        const VirtualShadowPlacement placement = virtualShadowPlacement(clipmap, level, camera);
        const Vec3 light{(texelAndDepth.x - placement.cameraTexelX) / placement.inverseTexel, (texelAndDepth.y - placement.cameraTexelY) / placement.inverseTexel,
                         (texelAndDepth.z - placement.depthOffset) / placement.depthScale};
        const Mat4 rotation = sunLightRotation(clipmap.lightDirection);
        Vec3 world = camera;

        for (int row = 0; row < 3; ++row) {
            const float along = row == 0 ? light.x : row == 1 ? light.y : light.z;
            world = world + Vec3{rotation.values[row][0], rotation.values[row][1], rotation.values[row][2]} * along;
        }

        return world;
    }

    std::uint32_t allLevels(int count) {
        return (1u << count) - 1u;
    }

    // Bad settings are refused, and every field reaches the settings file by its key.
    void testSettings() {
        validateVirtualShadows(VirtualShadowSettings{});

        VirtualShadowSettings bad;
        bad.firstHalfWidth = 0.0f;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));
        bad = {};
        bad.levelCount = 0;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));
        bad = {};
        bad.levelCount = maxVirtualShadowLevels + 1;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));
        bad = {};
        bad.poolPages = maxVirtualShadowPoolPages + 1;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));
        bad = {};
        bad.depthSlack = 1.0f;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));

        VirtualShadowSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        const std::vector<std::string> warnings = applySettings(
            {{"enabled", "true"}, {"showPages", "true"}, {"poolPages", "2048"}, {"firstLevelHalfWidth", "1"}, {"levels", "12"},
             {"levelResolution", "64 pages (8192)"}, {"depthSlack", "32"}},
            walk, StartupFields::Keep);
        assert(warnings.empty());
        assert(settings.enabled && settings.showPages && settings.poolPages == 2048);
        assert(settings.firstHalfWidth == 1.0f && settings.levelCount == 12);
        assert(settings.resolution == VirtualShadowResolution::Pages64 && settings.depthSlack == 32.0f);
        assert(virtualShadowPagesPerSide(VirtualShadowResolution::Pages32) == 32);
        assert(virtualShadowPagesPerSide(VirtualShadowResolution::Pages64) == 64);
    }

    // Each level twice the last; a page a 32nd of its width, a texel a 128th of that; the margin a page.
    void testLevelSizes() {
        assert(virtualShadowHalfWidth(0.5f, 0) == 0.5f);
        assert(virtualShadowHalfWidth(0.5f, 14) == 8192.0f);
        assert(virtualShadowPageWidth(0.5f, 32) == 1.0f / 32.0f);
        assert(virtualShadowTexelSize(0.5f, 32) == 1.0f / 4096.0f);
        assert(virtualShadowUsableHalfWidth(0.5f, 32) == 0.5f - 1.0f / 32.0f);
        assert(virtualShadowUsableHalfWidth(16.0f, 64) == 16.0f - 0.5f);
    }

    // The finest level that holds the distance: the very edge of a level's reach is its own, a hair past it the next;
    // past the last, none; zero the first.
    void testLevelChoice() {
        const float firstUsable = virtualShadowUsableHalfWidth(0.5f, 32);
        assert(virtualShadowLevelFor(0.0f, firstUsable, 15) == 0);

        for (int level = 0; level < 15; ++level) {
            const float reach = virtualShadowUsableHalfWidth(virtualShadowHalfWidth(0.5f, level), 32);
            assert(virtualShadowLevelFor(reach, firstUsable, 15) == level);
            const int beyond = virtualShadowLevelFor(std::nextafter(reach, 1e9f), firstUsable, 15);
            assert(beyond == (level + 1 < 15 ? level + 1 : virtualShadowNoLevel));
        }

        assert(virtualShadowLevelFor(1e6f, firstUsable, 15) == virtualShadowNoLevel);
        assert(virtualShadowLevelFor(1.0f, firstUsable, 1) == virtualShadowNoLevel);
    }

    /*
    * What the levels are for: a texel near a pixel's width everywhere. A pixel of a 1440p screen at 70 degrees is
    * d / 1028 wide at distance d; at 32 pages a level the chosen level's texel is between half and a little over one
    * pixel from a metre to 4 km (the first level is finer still nearer than that, the last level coarser past it).
    */
    void testTexelsFollowPixels() {
        const float pixelPerMetre = 2.0f * std::tan(0.5f * 70.0f * 3.14159265f / 180.0f) / 1440.0f;
        const float firstUsable = virtualShadowUsableHalfWidth(0.5f, 32);

        for (float distance = 1.0f; distance < 4000.0f; distance *= 1.01f) {
            const int level = virtualShadowLevelFor(distance, firstUsable, 15);
            assert(level != virtualShadowNoLevel);
            const float texel = virtualShadowTexelSize(virtualShadowHalfWidth(0.5f, level), 32);
            const float pixel = pixelPerMetre * distance;
            assert(texel >= 0.5f * pixel && texel <= 1.1f * pixel);
        }
    }

    /*
    * A point lands well inside its level: at least half a page from every edge (room for a soft shadow's filter), its
    * depth inside the range, and a caster the reach towards the sun still inside it. Cameras near and kilometres from
    * the origin, any sun, points in every direction at every level.
    */
    void testPointsLandInsideTheirLevel() {
        std::mt19937 random{7};
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const VirtualShadowSettings settings;
        const int pagesPerSide = virtualShadowPagesPerSide(settings.resolution);
        const float levelTexels = static_cast<float>(pagesPerSide * virtualShadowPageTexels);
        const float margin = 0.5f * static_cast<float>(virtualShadowPageTexels);
        const float firstUsable = virtualShadowUsableHalfWidth(settings.firstHalfWidth, pagesPerSide);

        for (int trial = 0; trial < 3000; ++trial) {
            const float far = trial % 2 == 0 ? 6000.0f : 30.0f;
            const Vec3 camera{(unit(random) - 0.5f) * far, unit(random) * 300.0f, (unit(random) - 0.5f) * far};
            const Vec3 sun = randomDirection(random);
            const VirtualShadowClipmap clipmap = freshClipmap(camera, sun, settings);

            const int level = trial % settings.levelCount;
            const float reach = virtualShadowUsableHalfWidth(virtualShadowHalfWidth(settings.firstHalfWidth, level), pagesPerSide);
            const Vec3 point = camera + randomDirection(random) * (reach * unit(random));
            const int chosen = virtualShadowLevelFor((point - camera).length(), firstUsable, settings.levelCount);
            assert(chosen != virtualShadowNoLevel && chosen <= level);

            for (int holding : {chosen, level}) {
                const Vec3 placed = inLevel(clipmap, holding, camera, point);
                assert(placed.x >= margin && placed.x <= levelTexels - margin);
                assert(placed.y >= margin && placed.y <= levelTexels - margin);
                assert(placed.z > 0.0f && placed.z < 1.0f);

                const Vec3 caster = inLevel(clipmap, holding, camera, point - sun * casterReach);
                assert(caster.z > placed.z && caster.z <= 1.0f);
            }

            // The worst case: the level's whole reach straight across the light, where the camera's half page off
            // the level's centre takes the margin's other half. Rounding allows a hundredth of a texel, and the
            // world's own: a point kilometres out is a float, placed to a few of the finest level's texels.
            const Mat4 rotation = sunLightRotation(sun);
            const float levelTexel = virtualShadowTexelSize(virtualShadowHalfWidth(settings.firstHalfWidth, level), pagesPerSide);
            const float worldRounding = 2.0f * (camera.length() + reach) * std::numeric_limits<float>::epsilon() / levelTexel;

            for (int axis = 0; axis < 2; ++axis) {
                const Vec3 across{rotation.values[axis][0], rotation.values[axis][1], rotation.values[axis][2]};

                for (float side : {-1.0f, 1.0f}) {
                    const Vec3 placed = inLevel(clipmap, level, camera, camera + across * (side * reach));
                    const float coordinate = axis == 0 ? placed.x : placed.y;
                    const float allowed = 0.01f + worldRounding;
                    assert(coordinate >= margin - allowed && coordinate <= levelTexels - margin + allowed);
                }
            }
        }
    }

    /*
    * Wrapping: as the camera walks, a point of the world keeps its page's table slot and its texel in the page, so a
    * cached page stays right while the level slides under it, and its depth stays the same while the range holds.
    */
    void testSlidingKeepsPages() {
        std::mt19937 random{11};
        std::uniform_real_distribution<float> unit(-1.0f, 1.0f);
        const VirtualShadowSettings settings;
        const int pagesPerSide = virtualShadowPagesPerSide(settings.resolution);
        const float firstUsable = virtualShadowUsableHalfWidth(settings.firstHalfWidth, pagesPerSide);

        for (int trial = 0; trial < 500; ++trial) {
            const Vec3 sun = randomDirection(random);
            const Vec3 start{unit(random) * 3000.0f, 100.0f + unit(random) * 50.0f, unit(random) * 3000.0f};
            std::optional<VirtualShadowClipmap> clipmap = freshClipmap(start, sun, settings);
            const int level = 3 + trial % 8;
            const float halfWidth = virtualShadowHalfWidth(settings.firstHalfWidth, level);
            const float texel = virtualShadowTexelSize(halfWidth, pagesPerSide);
            const Vec3 point = start + Vec3{unit(random), unit(random), unit(random)} * (0.2f * halfWidth);
            const Vec3 first = inLevel(*clipmap, level, start, point);
            const VirtualShadowPlacement firstPlacement = virtualShadowPlacement(*clipmap, level, start);
            const VirtualShadowPage firstPage = virtualShadowPageOf(first.x, first.y);
            const VirtualShadowPage firstSlot = virtualShadowSlot(firstPage, firstPlacement.originSlot, pagesPerSide);

            // Walk a few pages' worth across the level in steps; the point stays inside it.
            Vec3 camera = start;

            for (int step = 0; step < 6; ++step) {
                camera = camera + Vec3{unit(random), 0.1f * unit(random), unit(random)} * (0.04f * halfWidth);
                const VirtualShadowClipmapUpdate update = updateVirtualShadowClipmap(clipmap, camera, sun, settings, casterReach);
                assert((update.droppedLevels & (1u << level)) == 0);
                clipmap = update.clipmap;

                assert(virtualShadowLevelFor((point - camera).length(), firstUsable, settings.levelCount) <= level);
                const Vec3 now = inLevel(*clipmap, level, camera, point);
                const VirtualShadowPage page = virtualShadowPageOf(now.x, now.y);
                const VirtualShadowPage slot = virtualShadowSlot(page, virtualShadowPlacement(*clipmap, level, camera).originSlot, pagesPerSide);
                assert(slot == firstSlot);

                // Within the page, to a small part of a texel; float rounding of a point kilometres out is the limit.
                const float pageTexels = static_cast<float>(virtualShadowPageTexels);
                const float withinX = now.x - static_cast<float>(page.x) * pageTexels;
                const float firstWithinX = first.x - static_cast<float>(firstPage.x) * pageTexels;
                const float withinY = now.y - static_cast<float>(page.y) * pageTexels;
                const float firstWithinY = first.y - static_cast<float>(firstPage.y) * pageTexels;
                const float worldRounding = 4000.0f * std::numeric_limits<float>::epsilon() / texel;
                assert(std::abs(withinX - firstWithinX) < 0.01f + worldRounding);
                assert(std::abs(withinY - firstWithinY) < 0.01f + worldRounding);
                assert(std::abs(now.z - first.z) < 1e-5f + worldRounding * firstPlacement.depthScale * texel);
            }
        }
    }

    /*
    * Which levels lose every page: all for a new clipmap, a turned sun, new settings or a new caster reach; none for a
    * camera standing still or walking across the light; along the light, exactly the levels whose range no longer
    * holds it (slack - 1 half-widths).
    */
    void testDroppedLevels() {
        const VirtualShadowSettings settings;
        const Vec3 sun = Vec3{0.3f, -0.8f, 0.2f}.normalized();
        const Vec3 camera{120.0f, 40.0f, -75.0f};
        const VirtualShadowClipmapUpdate fresh = updateVirtualShadowClipmap(std::nullopt, camera, sun, settings, casterReach);
        assert(fresh.droppedLevels == allLevels(settings.levelCount));

        assert(updateVirtualShadowClipmap(fresh.clipmap, camera, sun, settings, casterReach).droppedLevels == 0);

        const Vec3 turned = Vec3{0.31f, -0.8f, 0.2f}.normalized();
        assert(updateVirtualShadowClipmap(fresh.clipmap, camera, turned, settings, casterReach).droppedLevels == allLevels(settings.levelCount));

        VirtualShadowSettings slacker = settings;
        slacker.depthSlack = 8.0f;
        assert(updateVirtualShadowClipmap(fresh.clipmap, camera, sun, slacker, casterReach).droppedLevels == allLevels(settings.levelCount));
        assert(updateVirtualShadowClipmap(fresh.clipmap, camera, sun, settings, 100.0f).droppedLevels == allLevels(settings.levelCount));

        // Across the light: a direction square to it.
        const Vec3 across = sun.cross(Vec3{0.0f, 1.0f, 0.0f}).normalized();
        assert(updateVirtualShadowClipmap(fresh.clipmap, camera + across * 500.0f, sun, settings, casterReach).droppedLevels == 0);

        for (float moved : {1.0f, 10.0f, 100.0f, 1000.0f}) {
            const VirtualShadowClipmapUpdate update = updateVirtualShadowClipmap(fresh.clipmap, camera + sun * moved, sun, settings, casterReach);
            std::uint32_t expected = 0;

            for (int level = 0; level < settings.levelCount; ++level) {
                const float holds = virtualShadowHalfWidth(settings.firstHalfWidth, level) * (settings.depthSlack - 1.0f);

                // Rounding of the moved camera's place along the light: only levels clearly past or short are checked.
                if (std::abs(moved - holds) < 1e-3f * holds) {
                    expected |= update.droppedLevels & (1u << level);
                } else if (moved > holds) {
                    expected |= 1u << level;
                }
            }

            assert(update.droppedLevels == expected);
        }
    }

    // Slots and table entries: every level's slots wrapped into the table, each entry once, pages left of the
    // world's origin as well as right of it.
    void testSlotsAndTable() {
        const int pagesPerSide = 32;
        std::vector<int> seen(static_cast<std::size_t>(3 * pagesPerSide * pagesPerSide), 0);

        for (int level = 0; level < 3; ++level) {
            for (int y = 0; y < pagesPerSide; ++y) {
                for (int x = 0; x < pagesPerSide; ++x) {
                    const VirtualShadowPage slot = virtualShadowSlot(VirtualShadowPage{x, y}, VirtualShadowPage{17, 5}, pagesPerSide);
                    assert(slot.x >= 0 && slot.x < pagesPerSide && slot.y >= 0 && slot.y < pagesPerSide);
                    ++seen[static_cast<std::size_t>(virtualShadowTableIndex(level, slot, pagesPerSide))];
                }
            }
        }

        for (int count : seen) {
            assert(count == 1);
        }

        assert((virtualShadowPageOf(127.9f, 128.0f) == VirtualShadowPage{0, 1}));
        assert((virtualShadowPageOf(-0.1f, 4095.9f) == VirtualShadowPage{-1, 31}));

        // A camera left of and below the world's origin across the light: its levels' first pages are negative, and
        // their slots still 0 to 31.
        const VirtualShadowClipmap clipmap = freshClipmap(Vec3{-5000.0f, 10.0f, -3000.0f}, Vec3{0.0f, -1.0f, 0.0f});

        for (int level = 0; level < 15; ++level) {
            const VirtualShadowPage origin = virtualShadowPlacement(clipmap, level, Vec3{-5000.0f, 10.0f, -3000.0f}).originSlot;
            assert(origin.x >= 0 && origin.x < pagesPerSide && origin.y >= 0 && origin.y < pagesPerSide);
        }
    }

    // Why each level has its own depth range: a depth step at the middle of the range (where the camera's points are)
    // stays below a quarter of the level's texel at every level; one range of 16 km for all would be 1 mm at the
    // finest level, nearly four of its texels.
    void testDepthStepsBelowATexel() {
        const VirtualShadowSettings settings;

        for (int level = 0; level < settings.levelCount; ++level) {
            const float halfWidth = virtualShadowHalfWidth(settings.firstHalfWidth, level);
            const float range = 2.0f * settings.depthSlack * halfWidth + casterReach;
            const float step = 0.5f * std::numeric_limits<float>::epsilon() * range;
            assert(step < 0.25f * virtualShadowTexelSize(halfWidth, 32));
        }

        assert(0.5f * std::numeric_limits<float>::epsilon() * 16000.0f > 3.0f * virtualShadowTexelSize(0.5f, 32));
    }
}

namespace {
    // A pixel marks its own page and those within the filter's reach: one inside a page, two by an edge, four by a
    // corner, never past the level's sides.
    void testMarkedPages() {
        const float reach = static_cast<float>(virtualShadowFilterTexels);
        const VirtualShadowPage none{};
        assert((virtualShadowMarkedPages(200.0f, 200.0f, reach, 32) == VirtualShadowPageSpan{VirtualShadowPage{1, 1}, VirtualShadowPage{1, 1}}));
        assert((virtualShadowMarkedPages(130.0f, 200.0f, reach, 32) == VirtualShadowPageSpan{VirtualShadowPage{0, 1}, VirtualShadowPage{1, 1}}));
        assert((virtualShadowMarkedPages(250.0f, 260.0f, reach, 32) == VirtualShadowPageSpan{VirtualShadowPage{1, 1}, VirtualShadowPage{2, 2}}));
        assert((virtualShadowMarkedPages(5.0f, 4090.0f, reach, 32) == VirtualShadowPageSpan{VirtualShadowPage{0, 31}, VirtualShadowPage{0, 31}}));
        assert((virtualShadowMarkedPages(100.0f, 100.0f, reach, 32).first == none));
    }

    // A slot's page is its page again, at every origin.
    void testSlotsTurnBackIntoPages() {
        for (int pagesPerSide : {32, 64}) {
            for (int origin = 0; origin < pagesPerSide; origin += 7) {
                for (int x = 0; x < pagesPerSide; ++x) {
                    const VirtualShadowPage page{x, (x * 5) % pagesPerSide};
                    const VirtualShadowPage originSlot{origin, (origin * 3) % pagesPerSide};
                    assert(virtualShadowPageOfSlot(virtualShadowSlot(page, originSlot, pagesPerSide), originSlot, pagesPerSide) == page);
                }
            }
        }
    }

    // Packing an entry, and the two questions resolving asks of it.
    void testEntries() {
        const std::uint32_t entry = virtualShadowEntryWith(4095, virtualShadowEntryAllocated | virtualShadowEntryDirty);
        assert(virtualShadowPhysicalPage(entry) == 4095 && (entry & virtualShadowEntryAllocated) != 0 && (entry & virtualShadowEntryDirty) != 0);
        assert(virtualShadowPhysicalPage(virtualShadowEntryWith(70000, 0)) == (70000u & 0xFFFFu));

        const VirtualShadowKey key{7, 9, 2};
        assert(!virtualShadowEntryStale(entry, key, key));
        assert(virtualShadowEntryStale(entry, key, VirtualShadowKey{8, 9, 2}));
        assert(virtualShadowEntryStale(entry, key, VirtualShadowKey{7, 9, 3}));
        assert(!virtualShadowEntryStale(0, key, VirtualShadowKey{8, 9, 2}));

        const std::uint32_t asked = virtualShadowEntryRequested;
        assert(virtualShadowEntryNeeds(asked, false));
        assert(!virtualShadowEntryNeeds(asked | entry, false));
        assert(virtualShadowEntryNeeds(asked | entry, true));
        assert(!virtualShadowEntryNeeds(entry, true));

        assert((virtualShadowAllocationSource(0, 3, 2) == VirtualShadowSource{VIRTUAL_SHADOW_FROM_FREE, 2}));
        assert((virtualShadowAllocationSource(2, 3, 2) == VirtualShadowSource{VIRTUAL_SHADOW_FROM_FREE, 0}));
        assert((virtualShadowAllocationSource(3, 3, 2) == VirtualShadowSource{VIRTUAL_SHADOW_FROM_CACHED, 0}));
        assert((virtualShadowAllocationSource(4, 3, 2) == VirtualShadowSource{VIRTUAL_SHADOW_FROM_CACHED, 1}));
        assert(virtualShadowAllocationSource(5, 3, 2).from == VIRTUAL_SHADOW_FROM_NOWHERE);
    }

    /*
    * The four passes on the CPU, with the functions they share with the shaders, in one order the GPU's atomics might
    * take: clear, mark (the entries asked for), resolve, allocate. Enough to follow pages over frames.
    */
    struct SimulatedPages {
        int levels = 0;
        int pagesPerSide = 0;
        std::vector<std::uint32_t> table;
        std::vector<VirtualShadowKey> keys;
        std::vector<std::uint32_t> freePages;
        std::uint32_t freeTop = 0;

        SimulatedPages(int levelCount, int sidePages, int pool):
            levels(levelCount), pagesPerSide(sidePages),
            table(static_cast<std::size_t>(levelCount * sidePages * sidePages), 0),
            keys(table.size()), freePages(static_cast<std::size_t>(pool)), freeTop(static_cast<std::uint32_t>(pool)) {
            for (int page = 0; page < pool; ++page) {
                freePages[static_cast<std::size_t>(page)] = static_cast<std::uint32_t>(page);
            }
        }

        VirtualShadowKey currentKey(const GpuVirtualShadowFrame& frame, int index) const {
            const int perLevel = pagesPerSide * pagesPerSide;
            const int level = index / perLevel;
            const VirtualShadowPage slot{(index % perLevel) % pagesPerSide, (index % perLevel) / pagesPerSide};
            const GpuVirtualShadowLevel& data = frame.levels[level];
            const VirtualShadowPage page = virtualShadowPageOfSlot(slot, VirtualShadowPage{data.pages[0], data.pages[1]}, pagesPerSide);
            return virtualShadowKeyOf(page, data.pages[2], data.pages[3], data.key[0]);
        }

        VirtualShadowCounts frame(const GpuVirtualShadowFrame& frame, const std::vector<int>& asked) {
            VirtualShadowCounts counts;

            for (std::uint32_t& entry : table) {
                entry &= ~(virtualShadowEntryRequested | virtualShadowEntryDirty | virtualShadowEntryFailed);
            }

            for (int index : asked) {
                table[static_cast<std::size_t>(index)] |= virtualShadowEntryRequested;
            }

            std::vector<int> needed;
            std::vector<int> cached;

            for (int index = 0; index < static_cast<int>(table.size()); ++index) {
                std::uint32_t& entry = table[static_cast<std::size_t>(index)];

                if (virtualShadowEntryStale(entry, keys[static_cast<std::size_t>(index)], currentKey(frame, index))) {
                    freePages[freeTop++] = virtualShadowPhysicalPage(entry);
                    ++counts.stale;
                    entry &= ~(virtualShadowEntryAllocated | 0xFFFFu);
                }

                const bool requested = (entry & virtualShadowEntryRequested) != 0;
                counts.requested += requested ? 1 : 0;

                if (virtualShadowEntryNeeds(entry, false)) {
                    needed.push_back(index);
                } else if (requested) {
                    ++counts.kept;
                } else if ((entry & virtualShadowEntryAllocated) != 0) {
                    cached.push_back(index);
                }
            }

            for (std::uint32_t k = 0; k < needed.size(); ++k) {
                const std::size_t index = static_cast<std::size_t>(needed[k]);
                const VirtualShadowSource source = virtualShadowAllocationSource(k, freeTop, static_cast<std::uint32_t>(cached.size()));
                std::uint32_t physical = 0;

                if (source.from == VIRTUAL_SHADOW_FROM_NOWHERE) {
                    table[index] |= virtualShadowEntryFailed;
                    ++counts.failed;
                    continue;
                }

                if (source.from == VIRTUAL_SHADOW_FROM_FREE) {
                    physical = freePages[source.index];
                } else {
                    std::uint32_t& victim = table[static_cast<std::size_t>(cached[source.index])];
                    physical = virtualShadowPhysicalPage(victim);
                    victim = 0;
                    ++counts.evicted;
                }

                table[index] = virtualShadowEntryWith(physical, virtualShadowEntryAllocated | virtualShadowEntryRequested | virtualShadowEntryDirty);
                keys[index] = currentKey(frame, static_cast<int>(index));
                ++counts.allocated;
            }

            freeTop -= std::min(freeTop, static_cast<std::uint32_t>(needed.size()));
            counts.free = freeTop;
            return counts;
        }

        // No physical page held by two entries, and none both held and free.
        bool consistent() const {
            std::vector<int> holders(freePages.size(), 0);

            for (std::uint32_t entry : table) {
                if ((entry & virtualShadowEntryAllocated) != 0) {
                    ++holders[virtualShadowPhysicalPage(entry)];
                }
            }

            for (std::uint32_t slot = 0; slot < freeTop; ++slot) {
                ++holders[freePages[slot]];
            }

            for (int count : holders) {
                if (count != 1) {
                    return false;
                }
            }

            return true;
        }
    };

    // The table entries a set of world points ask for this frame in one level, as the marking pass finds them (their
    // own page only); the points must be within the level's reach.
    std::vector<int> askedFor(const GpuVirtualShadowFrame& frame, const VirtualShadowClipmap& clipmap, const Vec3& camera, const std::vector<Vec3>& points,
                              int level) {
        std::vector<int> asked;
        const int pagesPerSide = frame.counts[1];

        for (const Vec3& point : points) {
            assert(virtualShadowLevelFor((point - camera).length(), frame.options[0], frame.counts[0]) <= level);
            const Vec3 placed = inLevel(clipmap, level, camera, point);
            const VirtualShadowPage slot = virtualShadowSlot(virtualShadowPageOf(placed.x, placed.y),
                                                             VirtualShadowPage{frame.levels[level].pages[0], frame.levels[level].pages[1]}, pagesPerSide);
            asked.push_back(virtualShadowTableIndex(level, slot, pagesPerSide));
        }

        std::sort(asked.begin(), asked.end());
        asked.erase(std::unique(asked.begin(), asked.end()), asked.end());
        return asked;
    }

    /*
    * Pages over frames: asked-for pages get places and keep them while asked for; a camera walking across the light
    * keeps every page still in its level (no stale ones) and gives new ones room; a dropped level frees its pages;
    * a pool too small evicts cached pages first and only then fails; no place is ever held twice.
    */
    void testPagesOverFrames() {
        VirtualShadowSettings settings;
        settings.levelCount = 4;
        settings.poolPages = 64;
        const Vec3 sun = Vec3{0.2f, -0.9f, 0.3f}.normalized();
        const Vec3 across = sun.cross(Vec3{0.0f, 1.0f, 0.0f}).normalized();
        std::array<std::uint32_t, maxVirtualShadowLevels> generations{};
        generations.fill(1);
        SimulatedPages pages{settings.levelCount, 32, settings.poolPages};

        // A row of points across the light near the camera, in level 2 (half-width 2 m, 4096 texels of 1 mm) all along
        // the walk below, a few pages wide.
        Vec3 camera{10.0f, 5.0f, -3.0f};
        std::optional<VirtualShadowClipmap> clipmap = freshClipmap(camera, sun, settings);
        std::vector<Vec3> points;

        for (int i = -10; i <= 10; ++i) {
            points.push_back(camera + across * (0.06f * static_cast<float>(i)) + Vec3{0.0f, -0.5f, 0.0f});
        }

        GpuVirtualShadowFrame frame = packVirtualShadowFrame(*clipmap, camera, generations, Mat4::identity(), 1.0f, 1.0f, false, 1);
        std::vector<int> asked = askedFor(frame, *clipmap, camera, points, 2);
        VirtualShadowCounts counts = pages.frame(frame, asked);
        assert(counts.requested == asked.size() && counts.allocated == asked.size() && counts.kept == 0 && counts.failed == 0);
        assert(counts.free == 64 - asked.size() && pages.consistent());

        counts = pages.frame(frame, asked);
        assert(counts.kept == asked.size() && counts.allocated == 0 && counts.stale == 0);

        // Walking: the camera slides a few pages across the light; the same world points keep their pages.
        for (int step = 0; step < 5; ++step) {
            camera = camera + across * 0.1f;
            const VirtualShadowClipmapUpdate update = updateVirtualShadowClipmap(clipmap, camera, sun, settings, casterReach);
            assert(update.droppedLevels == 0);
            clipmap = update.clipmap;
            frame = packVirtualShadowFrame(*clipmap, camera, generations, Mat4::identity(), 1.0f, 1.0f, false, 2);
            asked = askedFor(frame, *clipmap, camera, points, 2);
            counts = pages.frame(frame, asked);
            assert(counts.stale == 0 && counts.kept == asked.size() && counts.allocated == 0 && pages.consistent());
        }

        // A dropped level: every page of it stale, then given room again.
        ++generations[2];
        frame = packVirtualShadowFrame(*clipmap, camera, generations, Mat4::identity(), 1.0f, 1.0f, false, 3);
        counts = pages.frame(frame, asked);
        assert(counts.stale == asked.size() && counts.allocated == asked.size() && counts.kept == 0 && pages.consistent());

        // A crowd asking for more than the pool: the cached pages go first, then failures.
        std::vector<int> crowd;

        for (int index = 0; index < 100; ++index) {
            crowd.push_back(3 * 1024 + index);
        }

        counts = pages.frame(frame, crowd);
        assert(counts.allocated == 64 && counts.evicted == asked.size() && counts.failed == 100 - 64 && counts.free == 0);
        assert(pages.consistent());
    }

    // The frame's record: the clipmap's numbers placed for the camera, each level's generation and first page.
    void testFrameRecord() {
        VirtualShadowSettings settings;
        settings.levelCount = 6;
        settings.poolPages = 512;
        const Vec3 camera{-4000.5f, 30.0f, 2500.25f};
        const VirtualShadowClipmap clipmap = freshClipmap(camera, Vec3{0.1f, -1.0f, 0.2f}.normalized(), settings);
        std::array<std::uint32_t, maxVirtualShadowLevels> generations{};

        for (std::size_t level = 0; level < generations.size(); ++level) {
            generations[level] = static_cast<std::uint32_t>(10 + level);
        }

        const GpuVirtualShadowFrame frame = packVirtualShadowFrame(clipmap, camera, generations, Mat4::identity(), 1920.0f, 1080.0f, true, 77);
        assert(frame.counts[0] == 6 && frame.counts[1] == 32 && frame.counts[2] == 512 && frame.counts[3] == 77);
        assert(frame.options[0] == virtualShadowUsableHalfWidth(0.5f, 32) && frame.options[1] == 1.0f);
        assert(frame.screen[0] == 1920.0f && frame.screen[3] == 1.0f / 1080.0f);

        for (int level = 0; level < 6; ++level) {
            const VirtualShadowPlacement placement = virtualShadowPlacement(clipmap, level, camera);
            const GpuVirtualShadowLevel& packed = frame.levels[level];
            assert(packed.placement[0] == placement.cameraTexelX && packed.placement[2] == placement.inverseTexel);
            assert(packed.placement[3] == placement.depthScale && packed.depth[0] == placement.depthOffset);
            assert(packed.pages[0] == placement.originSlot.x && packed.pages[1] == placement.originSlot.y);
            assert(packed.pages[2] == static_cast<std::int32_t>(clipmap.levels[static_cast<std::size_t>(level)].firstPageX));
            assert(packed.key[0] == 10u + static_cast<std::uint32_t>(level));
        }

        assert(frame.levels[6].key[0] == 0);
    }

    // The counters by name, and F4's line under the tiles' while the pages are on.
    void testCountsAndStatsLine() {
        std::array<std::uint32_t, VIRTUAL_SHADOW_COUNTERS> counters{};
        counters[VIRTUAL_SHADOW_COUNT_REQUESTED] = 900;
        counters[VIRTUAL_SHADOW_COUNT_KEPT] = 850;
        counters[VIRTUAL_SHADOW_COUNT_ALLOCATED] = 50;
        counters[VIRTUAL_SHADOW_COUNT_EVICTED] = 7;
        counters[VIRTUAL_SHADOW_COUNT_FAILED] = 2;
        counters[VIRTUAL_SHADOW_COUNT_STALE] = 12;
        counters[VIRTUAL_SHADOW_COUNT_FREE] = 3100;
        const VirtualShadowCounts counts = virtualShadowCountsFrom(counters);
        assert(counts.requested == 900 && counts.kept == 850 && counts.allocated == 50 && counts.evicted == 7);
        assert(counts.failed == 2 && counts.stale == 12 && counts.free == 3100);

        RenderStats stats;
        const auto pagesLine = [](const std::vector<std::string>& lines) {
            return std::find_if(lines.begin(), lines.end(), [](const std::string& line) { return line.starts_with("VSM"); });
        };
        std::vector<std::string> lines = renderStatsLines(stats);
        assert(pagesLine(lines) == lines.end());
        stats.virtualShadowPages = counts;
        lines = renderStatsLines(stats);
        const auto line = pagesLine(lines);
        assert(line != lines.end() && *line == "VSM PAGES 900  KEPT 850  NEW 50 (EVICTED 7)  FAILED 2  STALE 12  FREE 3100");
        assert(line != lines.begin() && (line - 1)->starts_with("TILES"));

        // The sun's casters (stage 4c-i) on the line after: the draws its view took, clusters kept of those tested, their
        // triangles; no line without them.
        assert(std::none_of(lines.begin(), lines.end(), [](const std::string& text) { return text.starts_with("VSM CASTERS"); }));
        stats.virtualShadowCasters = VirtualShadowCasters{.draws = 1200, .tested = 500000, .kept = 80000, .triangles = 6400000};
        lines = renderStatsLines(stats);
        const auto casters = pagesLine(lines) + 1;
        assert(casters != lines.end() && *casters == "VSM CASTERS 1200 DRAWS  CLUSTERS 80000/500000  TRIS 6400K");
    }
}

namespace {
    /*
    * The casters' levels (stage 4c-i): up to the finest holding a surface that far along the view at the screen's
    * corner, which is farther from the camera than the same distance on the view's axis; every level past the last.
    */
    void testCasterLevels() {
        const VirtualShadowSettings settings;
        const int pagesPerSide = virtualShadowPagesPerSide(settings.resolution);
        const float firstUsable = virtualShadowUsableHalfWidth(settings.firstHalfWidth, pagesPerSide);
        const float fov = 70.0f * std::numbers::pi_v<float> / 180.0f;
        const float aspect = 16.0f / 9.0f;
        const float tanHalfHeight = std::tan(fov * 0.5f);
        const float stretch = std::sqrt(1.0f + tanHalfHeight * tanHalfHeight * (1.0f + aspect * aspect));

        assert(virtualShadowCasterLevels(settings, 0.0f, fov, aspect) == 1);
        assert(virtualShadowCasterLevels(settings, 1.0e6f, fov, aspect) == settings.levelCount);

        for (float along : {0.1f, 1.0f, 3.0f, 9.0f, 36.0f, 200.0f, 1000.0f, 3000.0f}) {
            const int levels = virtualShadowCasterLevels(settings, along, fov, aspect);
            const int corner = virtualShadowLevelFor(along * stretch * 0.9999f, firstUsable, settings.levelCount);
            assert(corner != virtualShadowNoLevel && levels > corner);
            assert(levels == virtualShadowLevelFor(along * stretch, firstUsable, settings.levelCount) + 1);
        }
    }

    /*
    * The casters' box holds every corner of every level it serves: each level's whole square across the light, from
    * its depth range's far end to its sunward end with the caster reach, after walks that leave the levels' depth
    * centres apart. Rounding allows a few floats of the box's own size. A page past the last level's side is outside.
    */
    void testCasterBoxHoldsItsLevels() {
        std::mt19937 random{11};
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        const VirtualShadowSettings settings;
        const int pagesPerSide = virtualShadowPagesPerSide(settings.resolution);
        const float side = static_cast<float>(pagesPerSide * virtualShadowPageTexels);

        for (int trial = 0; trial < 300; ++trial) {
            Vec3 camera{(unit(random) - 0.5f) * 4000.0f, unit(random) * 300.0f, (unit(random) - 0.5f) * 4000.0f};
            const Vec3 sun = randomDirection(random);
            std::optional<VirtualShadowClipmap> clipmap;

            for (int step = 0; step < 4; ++step) {
                camera = camera + randomDirection(random) * (unit(random) * 40.0f);
                clipmap = updateVirtualShadowClipmap(clipmap, camera, sun, settings, casterReach).clipmap;
            }

            const int levels = 1 + trial % settings.levelCount;
            const Frustum box = Frustum::fromClipMatrix(virtualShadowCasterBox(*clipmap, levels), ClipDepth::OneToZero);
            const float extent = camera.length() + 2.0f * settings.depthSlack * virtualShadowHalfWidth(settings.firstHalfWidth, levels - 1) + casterReach;
            const float allowed = 8.0f * extent * std::numeric_limits<float>::epsilon();

            for (int level = 0; level < levels; ++level) {
                for (int corner = 0; corner < 8; ++corner) {
                    const Vec3 texelAndDepth{(corner & 1) != 0 ? side : 0.0f, (corner & 2) != 0 ? side : 0.0f, (corner & 4) != 0 ? 1.0f : 0.0f};
                    const Vec3 point = levelPointInWorld(*clipmap, level, camera, texelAndDepth);

                    for (const Plane& plane : box.planes) {
                        assert(plane.signedDistance(point) >= -allowed);
                    }
                }
            }

            const float page = static_cast<float>(virtualShadowPageTexels);
            const Vec3 beyond = levelPointInWorld(*clipmap, levels - 1, camera, Vec3{side + page, 0.5f * side, 0.5f});
            assert(!box.intersectsSphere(beyond, 0.0f));
        }
    }
}

void testVirtualShadows() {
    testCasterLevels();
    testCasterBoxHoldsItsLevels();
    testMarkedPages();
    testSlotsTurnBackIntoPages();
    testEntries();
    testPagesOverFrames();
    testFrameRecord();
    testCountsAndStatsLine();
    testSettings();
    testLevelSizes();
    testLevelChoice();
    testTexelsFollowPixels();
    testPointsLandInsideTheirLevel();
    testSlidingKeepsPages();
    testDroppedLevels();
    testSlotsAndTable();
    testDepthStepsBelowATexel();
}
