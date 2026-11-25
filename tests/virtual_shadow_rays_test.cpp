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
    * A level and a scene of thin flat plates over flat ground, for marching rays over: texels of 1 mm (a level near
    * the camera), a depth range of 500 m, the receiver at texel (1000, 1000) and depth 0.5. A plate covers the texels
    * its test says at its height; the pool stores the highest surface at each texel's centre, as texelFetch reads it.
    */
    constexpr float inverseTexel = 1000.0f;
    constexpr float depthScale = 1.0f / 500.0f;
    constexpr float groundDepth = 0.5f;
    const Vec3 receiver{1000.0f, 1000.0f, groundDepth};

    struct Plate {
        float height = 0.0f;
        std::function<bool(float, float)> covers; // metres across from the receiver
    };

    std::function<std::optional<float>(float, float)> sceneOf(const std::vector<Plate>& plates) {
        return [plates](float texelX, float texelY) -> std::optional<float> {
            const float acrossX = (std::floor(texelX) + 0.5f - receiver.x) / inverseTexel;
            const float acrossY = (std::floor(texelY) + 0.5f - receiver.y) / inverseTexel;
            float stored = groundDepth;

            for (const Plate& plate : plates) {
                if (plate.covers(acrossX, acrossY)) {
                    stored = std::max(stored, groundDepth + plate.height * depthScale);
                }
            }

            return stored;
        };
    }

    float flatBias() {
        return virtualShadowBiasDepth(2.5f, inverseTexel, depthScale, 1.0f);
    }

    // Many rays and samples, so the march's answer is the disc's share to about a hundredth.
    VirtualShadowSettings fineRays() {
        VirtualShadowSettings settings;
        settings.rays = 64;
        settings.samplesPerRay = 32;
        return settings;
    }

    // A point's visibility averaged over several pixels' noise, as the temporal pass averages it.
    float averagedVisibility(const Vec3& point, const VirtualShadowSettings& settings, const std::function<std::optional<float>(float, float)>& scene,
                             float bias = flatBias()) {
        float sum = 0.0f;
        constexpr int pixels = 16;

        for (int pixel = 0; pixel < pixels; ++pixel) {
            const float turn = std::fmod(static_cast<float>(pixel) * 0.7548777f, 1.0f);
            const float jitter = std::fmod(static_cast<float>(pixel) * 0.5698403f + 0.25f, 1.0f);
            sum += virtualShadowRayVisibility(point, bias, tanSun, inverseTexel, depthScale, settings, turn, jitter, scene);
        }

        return sum / static_cast<float>(pixels);
    }

    // The share of a disc of radius 1 beyond a line c from its centre (the lit side past c), exactly.
    float discShareBeyond(float c) {
        if (c >= 1.0f) {
            return 0.0f;
        }

        if (c <= -1.0f) {
            return 1.0f;
        }

        return (std::acos(c) - c * std::sqrt(1.0f - c * c)) / std::numbers::pi_v<float>;
    }

    Vec3 receiverAcross(float metresX) {
        return Vec3{receiver.x + metresX * inverseTexel, receiver.y, groundDepth};
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
                    const float share = virtualShadowSampleShare(sample, samples, jitter);
                    assert(share >= 0.0f && share <= 1.0f && share > last);
                    last = share;
                }

                assert(last == 1.0f);
            }
        }

        // Squared: the first step is shorter than the one before the end.
        const float first = virtualShadowSampleShare(1, 8, 0.0f) - virtualShadowSampleShare(0, 8, 0.0f);
        const float late = virtualShadowSampleShare(6, 8, 0.0f) - virtualShadowSampleShare(5, 8, 0.0f);
        assert(first * 5.0f < late);
    }

    // A ray at the rim reaches exactly the reach's texels across at its end, unless the depth range ends first.
    void testRayHeightKeepsToTheReach() {
        for (float inverse : {4096.0f, 1000.0f, 10.0f, 0.5f}) {
            const float height = virtualShadowRayHeight(tanSun, inverse, 1e-5f, 0.5f);
            const Vec3 end = virtualShadowRaySample(receiver, Vec2{1.0f, 0.0f}, tanSun, inverse, 1e-5f, height);
            assert(std::abs(end.x - receiver.x - virtualShadowRayTexels) < 1e-3f);
            assert(std::abs(end.y - receiver.y) < 1e-6f);
        }

        // A coarse level whose range ends a metre up: the ray stops there, at depth 1.
        const float capped = virtualShadowRayHeight(tanSun, 0.5f, 0.01f, 0.99f);
        assert(std::abs(capped - 1.0f) < 1e-4f);
        const Vec3 end = virtualShadowRaySample(Vec3{0.0f, 0.0f, 0.99f}, Vec2{1.0f, 0.0f}, tanSun, 0.5f, 0.01f, capped);
        assert(std::abs(end.z - 1.0f) < 1e-5f);

        // No sun size: straight up to the range's end.
        assert(std::abs(virtualShadowRayHeight(0.0f, 1000.0f, depthScale, 0.5f) - 250.0f) < 1e-3f);
    }

    // Every sample of every ray lies on one of the point's four footprint pages, which are pages the point marked.
    void testSamplesStayOnTheMarkedPages() {
        std::mt19937 random{7};
        std::uniform_real_distribution<float> texel(130.0f, 3960.0f);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);

        for (int point = 0; point < 2000; ++point) {
            const Vec3 start{texel(random), texel(random), 0.4f};
            const float inverse = point % 2 == 0 ? 4096.0f : 3.0f;
            const float height = virtualShadowRayHeight(tanSun, inverse, 1e-4f, start.z);
            const VirtualShadowPage first = virtualShadowFootprintFirst(start.x, start.y);
            const VirtualShadowPageSpan marked = virtualShadowMarkedPages(start.x, start.y, static_cast<float>(virtualShadowFilterTexels), 32);

            for (int ray = 0; ray < 8; ++ray) {
                const Vec2 disc = virtualShadowRayDisc(ray, 8, unit(random));

                for (int sample = 0; sample < 8; ++sample) {
                    const float up = height * virtualShadowSampleShare(sample, 8, unit(random));
                    const Vec3 at = virtualShadowRaySample(start, disc, tanSun, inverse, 1e-4f, up);
                    const VirtualShadowPage page = virtualShadowPageOf(at.x, at.y);
                    const int index = virtualShadowFootprintIndex(page, first);
                    assert(index >= 0 && index <= 3);
                    assert(page.x - first.x <= 1 && page.y - first.y <= 1 && page.x >= first.x && page.y >= first.y);
                    assert(page.x >= marked.first.x && page.x <= marked.last.x && page.y >= marked.first.y && page.y <= marked.last.y);
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

        // Rising through it: blocked.
        VirtualShadowMarch through = virtualShadowMarchStep(march, 0.61f, 0.60f, tolerance, cap);
        assert(through.hit);

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

        // A hit stays a hit.
        assert(virtualShadowMarchStep(through, 0.9f, 0.1f, tolerance, cap).hit);
    }

    // A plate's edge h metres up: across the edge the march sees the share of the disc the plate leaves, h tan(a) its
    // radius there, exactly as a real half-plane would; the edge sharp at the plate's foot (contact hardening).
    void testEdgePenumbraIsTheDisc() {
        const VirtualShadowSettings settings = fineRays();

        for (float height : {0.25f, 1.0f, 2.0f}) {
            const float radius = height * tanSun;
            const auto scene = sceneOf({Plate{height, [](float x, float) { return x < 0.0f; }}});

            for (float c : {-1.5f, -0.8f, -0.4f, 0.0f, 0.4f, 0.8f, 1.5f}) {
                const float visibility = averagedVisibility(receiverAcross(c * radius), settings, scene);
                const float expected = discShareBeyond(c); // rays reaching the plate past its edge get through
                assert(std::abs(visibility - (1.0f - expected)) < 0.04f);
            }
        }

        // Narrower nearer the foot: just outside the low plate's penumbra, the high plate's still darkens the point.
        const auto low = sceneOf({Plate{0.25f, [](float x, float) { return x < 0.0f; }}});
        const auto high = sceneOf({Plate{2.0f, [](float x, float) { return x < 0.0f; }}});
        const float outside = 2.0f * 0.25f * tanSun;
        assert(averagedVisibility(receiverAcross(outside), settings, low) > 0.98f);
        assert(averagedVisibility(receiverAcross(outside), settings, high) < 0.9f);
    }

    // A leaf smaller than the sun's disc seen from below: the point under it sees the disc less the leaf, not a hard
    // core of shadow (a solid heightfield would black it out).
    void testSmallLeafLeavesThePointUnderItLit() {
        const float height = 2.0f;
        const float radius = height * tanSun;
        const float leaf = 0.4f * radius;
        const auto scene = sceneOf({Plate{height, [leaf](float x, float y) { return x * x + y * y < leaf * leaf; }}});
        const float visibility = averagedVisibility(receiver, fineRays(), scene);
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
        const auto scene = sceneOf({Plate{1.0f, [](float, float) { return true; }},
                                    Plate{2.0f, [strip0, strip1](float x, float) { return x > strip0 && x < strip1; }}});
        VirtualShadowSettings carried = fineRays();
        VirtualShadowSettings forgot = fineRays();
        forgot.extrapolateSlope = 0.0f;
        assert(averagedVisibility(receiver, carried, scene) < 0.02f);
        assert(averagedVisibility(receiver, forgot, scene) > 0.1f);
    }

    // Open ground, flat or sloping as far as the bias's cap, sees the whole sun: no acne from its own texels.
    void testOpenGroundIsLit() {
        assert(averagedVisibility(receiver, VirtualShadowSettings{}, sceneOf({})) == 1.0f);

        for (float degrees : {10.0f, 30.0f, 45.0f, 60.0f}) {
            const float rise = std::tan(degrees * std::numbers::pi_v<float> / 180.0f);
            const float nDotL = std::cos(degrees * std::numbers::pi_v<float> / 180.0f);
            const auto slope = [rise](float texelX, float texelY) -> std::optional<float> {
                const float across = (std::floor(texelX) + 0.5f - receiver.x + 0.3f * (std::floor(texelY) + 0.5f - receiver.y)) / inverseTexel;
                return groundDepth + rise * across * depthScale;
            };

            for (float offset : {0.0f, 0.37f, 0.81f}) {
                const Vec3 point{receiver.x + offset, receiver.y + 0.5f * offset, groundDepth + rise * (offset + 0.15f * offset) / inverseTexel * depthScale};
                const float bias = virtualShadowBiasDepth(2.5f, inverseTexel, depthScale, nDotL);
                assert(averagedVisibility(point, VirtualShadowSettings{}, slope, bias) == 1.0f);
            }
        }
    }

    // No rays: one hard comparison, 4c-iii's; a sample with no page is skipped.
    void testHardAndMissingPages() {
        VirtualShadowSettings hard;
        hard.rays = 0;
        const auto plate = sceneOf({Plate{1.0f, [](float x, float) { return x < 0.0f; }}});
        const float bias = flatBias();
        assert(virtualShadowRayVisibility(receiverAcross(-1e-3f), bias, tanSun, inverseTexel, depthScale, hard, 0.0f, 0.0f, plate) == 0.0f);
        assert(virtualShadowRayVisibility(receiverAcross(1e-3f), bias, tanSun, inverseTexel, depthScale, hard, 0.0f, 0.0f, plate) == 1.0f);

        const auto nothing = [](float, float) -> std::optional<float> { return std::nullopt; };
        assert(virtualShadowRayVisibility(receiver, bias, tanSun, inverseTexel, depthScale, VirtualShadowSettings{}, 0.3f, 0.6f, nothing) == 1.0f);
    }

    // The settings: defaults (Unreal's), their keys, the record's row, and what validation refuses.
    void testRaySettings() {
        const VirtualShadowSettings defaults;
        assert(defaults.rays == 8 && defaults.samplesPerRay == 8 && defaults.extrapolateSlope == 5.0f);

        VirtualShadowSettings settings;
        const SettingsWalk walk = [&settings](SettingsVisitor& visitor) {
            visit(settings, visitor);
        };
        const std::vector<std::string> warnings =
            applySettings({{"rays", "5"}, {"samplesPerRay", "3"}, {"extrapolateSlope", "2.5"}}, walk, StartupFields::Keep);
        assert(warnings.empty());
        assert(settings.rays == 5 && settings.samplesPerRay == 3 && settings.extrapolateSlope == 2.5f);

        const VirtualShadowClipmap clipmap = updateVirtualShadowClipmap(std::nullopt, Vec3{}, Vec3{0.0f, -1.0f, 0.0f}, settings, 200.0f).clipmap;
        std::array<std::uint32_t, maxVirtualShadowLevels> generations{};
        const GpuVirtualShadowFrame frame = packVirtualShadowFrame(clipmap, Vec3{}, generations, Mat4::identity(), 100.0f, 100.0f, 2.5f, 1);
        assert(frame.soft[0] == 5.0f && frame.soft[1] == 3.0f && frame.soft[2] == 2.5f);

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
    }
}

void testVirtualShadowRays() {
    testRayDiscCoversTheDisc();
    testSampleShares();
    testRayHeightKeepsToTheReach();
    testSamplesStayOnTheMarkedPages();
    testMarchSteps();
    testEdgePenumbraIsTheDisc();
    testSmallLeafLeavesThePointUnderItLit();
    testHiddenPlateIsCarriedOn();
    testOpenGroundIsLit();
    testHardAndMissingPages();
    testRaySettings();
}
