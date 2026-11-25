#include "core/settings_visitor.h"
#include "engine/settings_file.h"
#include "test_levels.h"
#include "math/mat4.h"
#include "render/gpu/cascades.h"
#include "render/gpu/virtual_shadow_rays.h"
#include "render/gpu/virtual_shadows.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <functional>
#include <numbers>
#include <optional>
#include <random>
#include <string>
#include <vector>


namespace {
    constexpr float tanSun = 0.00465f; // the sun's angular radius, DirectionalLight's

    /*
    * Levels and a scene of thin flat plates over ground, for marching rays through: the point's own level with texels
    * of a millimetre by default (a level near the camera), each next one twice as coarse with a depth range twice as
    * deep, its own texel for the scene's origin and its own ground depth, so a sample read in the wrong level, or
    * measured in the wrong one's units, lands somewhere else entirely. The pool stores the highest surface at each
    * texel's centre, as texelFetch reads it.
    */
    constexpr float depthScale = 1.0f / 500.0f;
    constexpr int levelCount = 6;

    struct TestLevel {
        float inverseTexel = 0.0f;
        float depthScale = 0.0f;
        Vec2 origin; // the scene's origin's texel
        float groundDepth = 0.0f;
    };

    TestLevel testLevel(int level, float baseInverseTexel) {
        const float coarsening = std::ldexp(1.0f, level);
        return TestLevel{baseInverseTexel / coarsening, depthScale / coarsening,
                         Vec2{1000.0f + 37.0f * static_cast<float>(level), 1000.0f - 11.0f * static_cast<float>(level)},
                         0.5f - 0.01f * static_cast<float>(level)};
    }

    // The point (metres across from the origin, standing height metres above the ground) in every level.
    std::vector<VirtualShadowRayLevel> levelsAt(float acrossX, float acrossY, float height = 0.0f, float baseInverseTexel = 1000.0f,
                                                float nDotL = 1.0f) {
        std::vector<VirtualShadowRayLevel> levels;

        for (int level = 0; level < levelCount; ++level) {
            const TestLevel in = testLevel(level, baseInverseTexel);
            const Vec3 point{in.origin.x + acrossX * in.inverseTexel, in.origin.y + acrossY * in.inverseTexel, in.groundDepth + height * in.depthScale};
            levels.push_back(VirtualShadowRayLevel{point, in.inverseTexel, in.depthScale, virtualShadowBiasDepth(2.5f, in.inverseTexel, in.depthScale, nDotL)});
        }

        return levels;
    }

    using HeightAt = std::function<float(float, float)>; // metres above the ground, at metres across from the origin
    using StoredAt = std::function<std::optional<float>(int, float, float)>;

    // The pool over a scene: levels listed in missing have no pages drawn.
    StoredAt storedOver(const HeightAt& heightAt, float baseInverseTexel = 1000.0f, std::vector<int> missing = {}) {
        return [heightAt, baseInverseTexel, missing](int level, float texelX, float texelY) -> std::optional<float> {
            if (std::find(missing.begin(), missing.end(), level) != missing.end()) {
                return std::nullopt;
            }

            const TestLevel in = testLevel(level, baseInverseTexel);
            const float acrossX = (std::floor(texelX) + 0.5f - in.origin.x) / in.inverseTexel;
            const float acrossY = (std::floor(texelY) + 0.5f - in.origin.y) / in.inverseTexel;
            return in.groundDepth + heightAt(acrossX, acrossY) * in.depthScale;
        };
    }

    struct Plate {
        float height = 0.0f;
        std::function<bool(float, float)> covers; // metres across from the origin
    };

    HeightAt plates(std::vector<Plate> list) {
        return [list](float x, float y) {
            float height = 0.0f;

            for (const Plate& plate : list) {
                if (plate.covers(x, y)) {
                    height = std::max(height, plate.height);
                }
            }

            return height;
        };
    }

    // Many rays and samples, so the march's answer is the disc's share to about a hundredth.
    VirtualShadowSettings fineRays(int rayLevels = 4) {
        VirtualShadowSettings settings;
        settings.rays = 64;
        settings.samplesPerRay = 32;
        settings.rayLevels = rayLevels;
        return settings;
    }

    // A point's visibility averaged over several pixels' noise, as the temporal pass averages it.
    float averagedVisibility(const std::vector<VirtualShadowRayLevel>& levels, const VirtualShadowSettings& settings, const StoredAt& stored) {
        float sum = 0.0f;
        constexpr int pixels = 16;

        for (int pixel = 0; pixel < pixels; ++pixel) {
            const float turn = std::fmod(static_cast<float>(pixel) * 0.7548777f, 1.0f);
            const float jitter = std::fmod(static_cast<float>(pixel) * 0.5698403f + 0.25f, 1.0f);
            sum += virtualShadowRayVisibility(levels, tanSun, settings, turn, jitter, stored);
        }

        return sum / static_cast<float>(pixels);
    }

    // The share of a disc of radius 1 beyond a line c from its centre, exactly.
    float discShareBeyond(float c) {
        if (c >= 1.0f) {
            return 0.0f;
        }

        if (c <= -1.0f) {
            return 1.0f;
        }

        return (std::acos(c) - c * std::sqrt(1.0f - c * c)) / std::numbers::pi_v<float>;
    }

    // Across a half-plane plate's edge (covering x < 0, height metres up): the march against the disc's exact share at
    // c penumbra radii from the edge (rays reaching the plate past its edge get through).
    void checkEdge(float height, const VirtualShadowSettings& settings, float baseInverseTexel, std::vector<int> missing, float tolerance) {
        const float radius = height * tanSun;
        const StoredAt scene = storedOver(plates({Plate{height, [](float x, float) { return x < 0.0f; }}}), baseInverseTexel, missing);

        for (float c : {-1.5f, -0.8f, -0.4f, 0.0f, 0.4f, 0.8f, 1.5f}) {
            const float visibility = averagedVisibility(levelsAt(c * radius, 0.0f, 0.0f, baseInverseTexel), settings, scene);
            assert(std::abs(visibility - (1.0f - discShareBeyond(c))) < tolerance);
        }
    }
}

namespace {
    // Rays cover the disc evenly: inside it, their squared radii averaging a half (an even disc's), turned by the turn.
    void testRayDiscCoversTheDisc() {
        for (int rays : {1, 4, 8, 64}) {
            float squared = 0.0f;

            for (int ray = 0; ray < rays; ++ray) {
                const Vec2 point = virtualShadowRayDisc(ray, rays, 0.0f);
                const float radius = std::sqrt(point.x * point.x + point.y * point.y);
                assert(radius <= 1.0f + 1e-6f);
                squared += radius * radius;

                const Vec2 turned = virtualShadowRayDisc(ray, rays, 0.25f);
                assert(std::abs(turned.x * turned.x + turned.y * turned.y - radius * radius) < 1e-5f);
                assert(std::abs(turned.x + point.y) < 1e-4f && std::abs(turned.y - point.x) < 1e-4f);
            }

            assert(std::abs(squared / static_cast<float>(rays) - 0.5f) < 1e-5f);
        }

        // Every quarter of the disc gets its share of 64 rays.
        std::array<int, 4> quarters{};

        for (int ray = 0; ray < 64; ++ray) {
            const Vec2 point = virtualShadowRayDisc(ray, 64, 0.1f);
            ++quarters[static_cast<std::size_t>((point.x >= 0.0f ? 0 : 1) + (point.y >= 0.0f ? 0 : 2))];
        }

        for (int count : quarters) {
            assert(count >= 13 && count <= 19);
        }
    }

    // Samples climb the ray, nearer together near the point, the last at its end.
    void testSampleShares() {
        for (int samples : {1, 2, 8, 32}) {
            for (float jitter : {0.0f, 0.5f, 0.999f}) {
                float last = -1.0f;

                for (int sample = 0; sample < samples; ++sample) {
                    const float share = virtualShadowSampleShare(sample, samples, jitter, 0);
                    assert(share >= 0.0f && share <= 1.0f && share > last);
                    last = share;
                }

                assert(last == 1.0f);
            }
        }

        // Squared: the first step is shorter than the one before the end.
        const float first = virtualShadowSampleShare(1, 8, 0.0f, 0) - virtualShadowSampleShare(0, 8, 0.0f, 0);
        const float late = virtualShadowSampleShare(6, 8, 0.0f, 0) - virtualShadowSampleShare(5, 8, 0.0f, 0);
        assert(first * 5.0f < late);

        // Rays four levels long: half the samples within the point's own level's reach, a sixteenth of the ray.
        assert(std::abs(virtualShadowSampleShare(4, 8, 0.0f, 4) - 1.0f / 16.0f) < 1e-7f);
        assert(virtualShadowSampleShare(3, 8, 0.5f, 4) < 1.0f / 16.0f && virtualShadowSampleShare(4, 8, 0.5f, 4) > 1.0f / 16.0f);
        assert(virtualShadowSampleShare(4, 8, 0.0f, 1) == virtualShadowSampleShare(4, 8, 0.0f, 0));
    }

    // A ray at the rim reaches exactly the reach's texels across at its end (12 x 2^levelsUp of its level's), unless
    // the depth range ends first.
    void testRayHeightKeepsToTheReach() {
        const Vec3 start{1000.0f, 1000.0f, 0.5f};

        for (int levelsUp : {0, 1, 4}) {
            for (float inverse : {4096.0f, 1000.0f, 10.0f, 0.5f}) {
                const float height = virtualShadowRayHeight(tanSun, inverse, 1e-6f, 0.5f, levelsUp);
                const Vec3 end = virtualShadowRaySample(start, Vec2{1.0f, 0.0f}, tanSun, inverse, 1e-6f, height);
                assert(std::abs(end.x - start.x - std::ldexp(virtualShadowRayTexels, levelsUp)) < 2e-3f * std::ldexp(1.0f, levelsUp));
                assert(std::abs(end.y - start.y) < 1e-6f);
            }
        }

        // A coarse level whose range ends a metre up: the ray stops there, at depth 1.
        const float capped = virtualShadowRayHeight(tanSun, 0.5f, 0.01f, 0.99f, 4);
        assert(std::abs(capped - 1.0f) < 1e-4f);
        const Vec3 end = virtualShadowRaySample(Vec3{0.0f, 0.0f, 0.99f}, Vec2{1.0f, 0.0f}, tanSun, 0.5f, 0.01f, capped);
        assert(std::abs(end.z - 1.0f) < 1e-5f);

        // No sun size: straight up to the range's end.
        assert(std::abs(virtualShadowRayHeight(0.0f, 1000.0f, depthScale, 0.5f, 0) - 250.0f) < 1e-3f);
    }

    // A sample's level: the first where its offset is within 12 texels, by doubling, at most levelsUp; a stored depth
    // as a height above the point in its level's own scale.
    void testSampleLevels() {
        assert(virtualShadowSampleLevel(0.0f, 4) == 0);
        assert(virtualShadowSampleLevel(12.0f, 4) == 0);
        assert(virtualShadowSampleLevel(std::nextafter(12.0f, 13.0f), 4) == 1);
        assert(virtualShadowSampleLevel(24.0f, 4) == 1);
        assert(virtualShadowSampleLevel(48.0f, 4) == 2);
        assert(virtualShadowSampleLevel(49.0f, 4) == 3);
        assert(virtualShadowSampleLevel(96.0f, 4) == 3);
        assert(virtualShadowSampleLevel(100.0f, 4) == 4);
        assert(virtualShadowSampleLevel(1e6f, 4) == 4);
        assert(virtualShadowSampleLevel(1e6f, 0) == 0);

        assert(std::abs(virtualShadowStoredHeight(0.6f, 0.5f, 0.01f) - 10.0f) < 1e-4f);
        assert(std::abs(virtualShadowStoredHeight(0.45f, 0.5f, 0.02f) + 2.5f) < 1e-4f);
    }

    /*
    * Every sample of every ray lies within 12 texels of its point in the level it reads, so on pages the point marked
    * there; in the point's own level, on one of its four footprint pages.
    */
    void testSamplesStayOnTheMarkedPages() {
        std::mt19937 random{7};
        std::uniform_real_distribution<float> texel(130.0f, 3960.0f);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);

        for (int point = 0; point < 2000; ++point) {
            const int levelsUp = point % 3 == 0 ? 0 : 4;
            const Vec3 start{texel(random), texel(random), 0.4f};
            const float inverse = point % 2 == 0 ? 4096.0f : 3.0f;
            const float height = virtualShadowRayHeight(tanSun, inverse, 1e-5f, start.z, levelsUp);
            const VirtualShadowPage first = virtualShadowFootprintFirst(start.x, start.y);
            const VirtualShadowPageSpan marked = virtualShadowMarkedPages(start.x, start.y, static_cast<float>(virtualShadowFilterTexels), 32);

            for (int ray = 0; ray < 8; ++ray) {
                const Vec2 disc = virtualShadowRayDisc(ray, 8, unit(random));
                const float discReach = std::sqrt(disc.x * disc.x + disc.y * disc.y);

                for (int sample = 0; sample < 8; ++sample) {
                    const float up = height * virtualShadowSampleShare(sample, 8, unit(random), levelsUp);
                    const float offset = discReach * up * tanSun * inverse;
                    const int level = virtualShadowSampleLevel(offset, levelsUp);
                    assert(offset <= std::ldexp(virtualShadowRayTexels, level) * 1.0001f);

                    if (level == 0) {
                        const Vec3 at = virtualShadowRaySample(start, disc, tanSun, inverse, 1e-5f, up);
                        const VirtualShadowPage page = virtualShadowPageOf(at.x, at.y);
                        const int index = virtualShadowFootprintIndex(page, first);
                        assert(index >= 0 && index <= 3);
                        assert(page.x - first.x <= 1 && page.y - first.y <= 1 && page.x >= first.x && page.y >= first.y);
                        assert(page.x >= marked.first.x && page.x <= marked.last.x && page.y >= marked.first.y && page.y <= marked.last.y);
                    }
                }
            }
        }

        assert(virtualShadowFootprintIndex(VirtualShadowPage{4, 6}, VirtualShadowPage{3, 6}) == 1);
        assert(virtualShadowFootprintIndex(VirtualShadowPage{3, 7}, VirtualShadowPage{3, 6}) == 2);
        assert(virtualShadowFootprintIndex(VirtualShadowPage{4, 7}, VirtualShadowPage{3, 6}) == 3);
    }

    // The march's rules one sample at a time (reversed depths: larger is sunwards).
    void testMarchSteps() {
        const float tolerance = 0.001f;
        const float cap = 0.02f;
        VirtualShadowMarch march;

        // In the open: nothing.
        march = virtualShadowMarchStep(march, 0.50f, 0.49f, tolerance, cap);
        assert(!march.under && !march.hit);

        // Under a plate: not blocked for being under it.
        march = virtualShadowMarchStep(march, 0.51f, 0.60f, tolerance, cap);
        assert(march.under && !march.hit && march.surfaceDepth == 0.60f);

        // Out above its height: the crossing is read. The plate there: blocked (through it); the ground there: not
        // (out beside it before rising to its height); no page there: blocked (the guess).
        const VirtualShadowMarch rising = virtualShadowMarchStep(march, 0.61f, 0.50f, tolerance, cap);
        assert(rising.checking && rising.checkDepth == 0.60f && !rising.hit && !rising.under);
        assert(virtualShadowCrossingDepth(rising, 0.51f, 0.61f) == 0.60f);
        assert(virtualShadowCrossingDepth(rising, 0.40f, 0.55f) == 0.55f);
        const VirtualShadowMarch through = virtualShadowMarchCheck(rising, 0.60f, tolerance);
        assert(through.hit && !through.checking);
        assert(!virtualShadowMarchCheck(rising, 0.50f, tolerance).hit);
        assert(virtualShadowMarchCheck(rising, std::nullopt, tolerance).hit);

        // Out in the open past a plate higher than the ray was at the last sample: the crossing is read too.
        VirtualShadowMarch open;
        open.lastRayDepth = 0.495f; // above the ground at the last sample too
        open = virtualShadowMarchStep(open, 0.50f, 0.49f, tolerance, cap);
        assert(!open.checking);
        const VirtualShadowMarch past = virtualShadowMarchStep(open, 0.70f, 0.60f, tolerance, cap);
        assert(past.checking && past.checkDepth == 0.60f && past.lastRayDepth == 0.70f);
        assert(!virtualShadowMarchStep(open, 0.70f, 0.50f, tolerance, cap).checking);

        // Slipping out sideways beneath it to the ground: not blocked, out in the open again.
        VirtualShadowMarch beside = virtualShadowMarchStep(march, 0.52f, 0.50f, tolerance, cap);
        assert(!beside.hit && !beside.under);
        assert(!virtualShadowRayBlocked(beside));

        // Ending still under it: blocked (a taller caster).
        assert(virtualShadowRayBlocked(march));

        // A higher plate hiding it: carried on, and met when the ray rises to it.
        VirtualShadowMarch hidden = virtualShadowMarchStep(march, 0.55f, 0.80f, tolerance, cap);
        assert(hidden.under && !hidden.hit && hidden.surfaceDepth == 0.60f);
        hidden = virtualShadowMarchStep(hidden, 0.605f, 0.80f, tolerance, cap);
        assert(hidden.hit);

        // Without carrying on, the higher plate replaces it and the ray slips past the hidden one.
        VirtualShadowMarch forgot = virtualShadowMarchStep(march, 0.55f, 0.80f, tolerance, 0.0f);
        assert(forgot.surfaceDepth == 0.80f);
        forgot = virtualShadowMarchStep(forgot, 0.605f, 0.80f, tolerance, 0.0f);
        assert(!forgot.hit);

        // The plate rising a little as the ray goes on beneath it (within the bias and the slope cap): the same plate,
        // its slope taken (capped), not a higher one hiding it.
        VirtualShadowMarch sloping = virtualShadowMarchStep(march, 0.58f, 0.602f, tolerance, cap);
        assert(sloping.under && !sloping.hit && sloping.surfaceDepth == 0.602f && sloping.surfaceSlope == cap);

        // A step clears a crossing asked for before (the march reads each at once).
        VirtualShadowMarch stale = rising;
        stale.lastRayDepth = 0.62f;
        assert(!virtualShadowMarchStep(stale, 0.63f, 0.50f, tolerance, cap).checking);

        // A hit stays a hit.
        assert(virtualShadowMarchStep(through, 0.9f, 0.1f, tolerance, cap).hit);
    }

    // A plate's edge h metres up: across the edge the march sees the share of the disc the plate leaves, h tan(a) its
    // radius there, exactly as a real half-plane would; the edge sharp at the plate's foot (contact hardening).
    void testEdgePenumbraIsTheDisc() {
        for (float height : {0.25f, 1.0f, 2.0f}) {
            checkEdge(height, fineRays(), 1000.0f, {}, 0.02f);
        }

        // Narrower nearer the foot: just outside the low plate's penumbra, the high plate's still darkens the point.
        const StoredAt low = storedOver(plates({Plate{0.25f, [](float x, float) { return x < 0.0f; }}}));
        const StoredAt high = storedOver(plates({Plate{2.0f, [](float x, float) { return x < 0.0f; }}}));
        const float outside = 2.0f * 0.25f * tanSun;
        assert(averagedVisibility(levelsAt(outside, 0.0f), fineRays(), low) > 0.98f);
        assert(averagedVisibility(levelsAt(outside, 0.0f), fineRays(), high) < 0.9f);
    }

    /*
    * A caster 100 m up near the camera (4 mm texels): its 47 cm penumbra is 116 of the point's texels, past one level's
    * 12, so the rays read coarser levels and still see the disc; kept to the point's level (rayLevels 0) the edge is
    * sharp, a ray never reaching the plate's height there.
    */
    void testWidePenumbraReadsCoarserLevels() {
        checkEdge(100.0f, fineRays(4), 250.0f, {}, 0.02f);

        const float radius = 100.0f * tanSun;
        const StoredAt scene = storedOver(plates({Plate{100.0f, [](float x, float) { return x < 0.0f; }}}), 250.0f);
        assert(averagedVisibility(levelsAt(0.4f * radius, 0.0f, 0.0f, 250.0f), fineRays(0), scene) > 0.95f);
        assert(averagedVisibility(levelsAt(-0.4f * radius, 0.0f, 0.0f, 250.0f), fineRays(0), scene) < 0.05f);
    }

    /*
    * Every read a wide ray makes lies within 12 texels of the point in the level it reads (where the point marked its
    * pages): its samples, the crossings, and the coarser levels read for missing pages.
    */
    void testReadsStayOnMarkedPages() {
        const float radius = 100.0f * tanSun;
        const StoredAt scene = storedOver(plates({Plate{100.0f, [](float x, float) { return x < 0.0f; }}}), 250.0f, {1});

        for (float c : {-0.6f, 0.0f, 0.7f}) {
            const std::vector<VirtualShadowRayLevel> levels = levelsAt(c * radius, 0.0f, 0.0f, 250.0f);
            int reads = 0;
            const StoredAt watched = [&](int level, float x, float y) {
                const Vec3& point = levels[static_cast<std::size_t>(level)].point;
                assert(std::max(std::abs(x - point.x), std::abs(y - point.y)) <= virtualShadowRayTexels * 1.001f);
                ++reads;
                return scene(level, x, y);
            };
            averagedVisibility(levels, fineRays(4), watched);
            assert(reads > 0);
        }
    }

    // A level with no pages drawn: its samples read the next coarser level instead, and the penumbra is the same.
    void testMissingPagesReadCoarserLevels() {
        checkEdge(2.0f, fineRays(), 1000.0f, {0}, 0.02f);
        checkEdge(100.0f, fineRays(4), 250.0f, {2, 3}, 0.02f);
    }

    // A leaf smaller than the sun's disc seen from below: the point under it sees the disc less the leaf, not a hard
    // core of shadow (a solid heightfield would black it out).
    void testSmallLeafLeavesThePointUnderItLit() {
        const float height = 2.0f;
        const float radius = height * tanSun;
        const float leaf = 0.4f * radius;
        const StoredAt scene = storedOver(plates({Plate{height, [leaf](float x, float y) { return x * x + y * y < leaf * leaf; }}}));
        const float visibility = averagedVisibility(levelsAt(0.0f, 0.0f), fineRays(), scene);
        assert(std::abs(visibility - (1.0f - 0.16f)) < 0.06f);
    }

    /*
    * Two plates overlapping: a wide one at 1 m over everything, a strip at 2 m hiding part of it. Every ray from the
    * ground meets the low plate. Rays passing under the strip once it hides the low plate would slip past it and leak
    * light, unless the low plate is carried on behind the strip (the extrapolation).
    */
    void testHiddenPlateIsCarriedOn() {
        const float strip0 = 0.5e-3f;
        const float strip1 = 6.0e-3f;
        const StoredAt scene = storedOver(plates({Plate{1.0f, [](float, float) { return true; }},
                                                  Plate{2.0f, [strip0, strip1](float x, float) { return x > strip0 && x < strip1; }}}));
        VirtualShadowSettings carried = fineRays(0);
        VirtualShadowSettings forgot = fineRays(0);
        forgot.extrapolateSlope = 0.0f;
        assert(averagedVisibility(levelsAt(0.0f, 0.0f), carried, scene) < 0.02f);
        assert(averagedVisibility(levelsAt(0.0f, 0.0f), forgot, scene) > 0.1f);
    }

    // Open ground, flat or sloping as far as the bias's cap, sees the whole sun: no acne from its own texels.
    void testOpenGroundIsLit() {
        assert(averagedVisibility(levelsAt(0.0f, 0.0f), VirtualShadowSettings{}, storedOver(plates({}))) == 1.0f);

        for (float degrees : {10.0f, 30.0f, 45.0f, 60.0f}) {
            const float rise = std::tan(degrees * std::numbers::pi_v<float> / 180.0f);
            const float nDotL = std::cos(degrees * std::numbers::pi_v<float> / 180.0f);
            const StoredAt slope = storedOver([rise](float x, float y) { return rise * (x + 0.3f * y); });

            for (float offset : {0.0f, 0.37e-3f, 0.81e-3f}) {
                const float x = offset;
                const float y = 0.5f * offset;
                assert(averagedVisibility(levelsAt(x, y, rise * (x + 0.3f * y), 1000.0f, nDotL), VirtualShadowSettings{}, slope) == 1.0f);
            }
        }
    }

    // No rays: one hard comparison, 4c-iii's; a sample with no page anywhere is skipped.
    void testHardAndMissingPages() {
        VirtualShadowSettings hard;
        hard.rays = 0;
        const StoredAt plate = storedOver(plates({Plate{1.0f, [](float x, float) { return x < 0.0f; }}}));
        assert(virtualShadowRayVisibility(levelsAt(-1e-3f, 0.0f), tanSun, hard, 0.0f, 0.0f, plate) == 0.0f);
        assert(virtualShadowRayVisibility(levelsAt(1e-3f, 0.0f), tanSun, hard, 0.0f, 0.0f, plate) == 1.0f);

        const StoredAt nothing = [](int, float, float) -> std::optional<float> { return std::nullopt; };
        assert(virtualShadowRayVisibility(levelsAt(0.0f, 0.0f), tanSun, VirtualShadowSettings{}, 0.3f, 0.6f, nothing) == 1.0f);
    }

    // The settings: defaults (Unreal's), their keys, the record's row, and what validation refuses.
    void testRaySettings() {
        const VirtualShadowSettings defaults;
        assert(defaults.rays == 8 && defaults.samplesPerRay == 8 && defaults.extrapolateSlope == 5.0f && defaults.rayLevels == 4);

        VirtualShadowSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        const std::vector<std::string> warnings =
            applySettings({{"rays", "5"}, {"samplesPerRay", "3"}, {"extrapolateSlope", "2.5"}, {"rayLevels", "2"}}, walk, StartupFields::Keep);
        assert(warnings.empty());
        assert(settings.rays == 5 && settings.samplesPerRay == 3 && settings.extrapolateSlope == 2.5f && settings.rayLevels == 2);

        const VirtualShadowClipmap clipmap = updateVirtualShadowClipmap(std::nullopt, Vec3{}, Vec3{0.0f, -1.0f, 0.0f}, settings, 200.0f).clipmap;
        std::array<std::uint32_t, maxVirtualShadowLevels> generations{};
        const GpuVirtualShadowFrame frame = packVirtualShadowFrame(clipmap, Vec3{}, generations, Mat4::identity(), 100.0f, 100.0f, 2.5f, 1);
        assert(frame.soft[0] == 5.0f && frame.soft[1] == 3.0f && frame.soft[2] == 2.5f && frame.soft[3] == 2.0f);

        // Without rays no coarser levels are read, so none are marked.
        settings.rays = 0;
        const VirtualShadowClipmap hard = updateVirtualShadowClipmap(std::nullopt, Vec3{}, Vec3{0.0f, -1.0f, 0.0f}, settings, 200.0f).clipmap;
        assert(packVirtualShadowFrame(hard, Vec3{}, generations, Mat4::identity(), 100.0f, 100.0f, 2.5f, 1).soft[3] == 0.0f);

        VirtualShadowSettings bad;
        bad.rays = -1;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));
        bad = {};
        bad.rays = maxVirtualShadowRays + 1;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));
        bad = {};
        bad.samplesPerRay = 0;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));
        bad = {};
        bad.extrapolateSlope = -1.0f;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));
        bad = {};
        bad.rayLevels = maxVirtualShadowRayLevels + 1;
        assert(throwsInvalid([&] { validateVirtualShadows(bad); }));
    }
}

void testVirtualShadowRays() {
    testRayDiscCoversTheDisc();
    testSampleShares();
    testRayHeightKeepsToTheReach();
    testSampleLevels();
    testSamplesStayOnTheMarkedPages();
    testMarchSteps();
    testEdgePenumbraIsTheDisc();
    testWidePenumbraReadsCoarserLevels();
    testMissingPagesReadCoarserLevels();
    testReadsStayOnMarkedPages();
    testSmallLeafLeavesThePointUnderItLit();
    testHiddenPlateIsCarriedOn();
    testOpenGroundIsLit();
    testHardAndMissingPages();
    testRaySettings();
}
