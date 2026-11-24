#include "engine/settings_file.h"
#include "math/mat4.h"
#include "render/gpu/cascades.h"
#include "render/gpu/virtual_shadows.h"
#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
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
        bad.depthSlack = 1.0f;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));

        VirtualShadowSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        const std::vector<std::string> warnings = applySettings(
            {{"firstLevelHalfWidth", "1"}, {"levels", "12"}, {"levelResolution", "64 pages (8192)"}, {"depthSlack", "32"}}, walk, StartupFields::Keep);
        assert(warnings.empty());
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

void testVirtualShadows() {
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
