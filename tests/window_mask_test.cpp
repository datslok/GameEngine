#include "render/gpu/window_mask.h"

#include <cassert>
#include <cmath>
#include <cstdint>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::fabs(a - b) <= tolerance;
    }

    bool same(const Vec3& a, const Vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    /*
    * The slits' running cover: 0 at a cell's start, each whole cell adding the slit's length, flat outside the slit and
    * rising one for one inside it, the same a cell further on less one length, and below zero as well.
    */
    void testPulseIntegral() {
        const float pitch = 6.0f;
        const float length = 3.0f;
        assert(pulseIntegral(0.0f, pitch, length) == 0.0f && pulseIntegral(1.5f, pitch, length) == 0.0f);
        assert(near(pulseIntegral(3.0f, pitch, length), 1.5f, 1e-6f) && near(pulseIntegral(4.5f, pitch, length), 3.0f, 1e-6f));
        assert(near(pulseIntegral(6.0f, pitch, length), 3.0f, 1e-6f));

        for (float x = -40.0f; x < 40.0f; x += 0.37f) {
            assert(near(pulseIntegral(x + pitch, pitch, length) - pulseIntegral(x, pitch, length), length, 1e-4f));
            assert(pulseIntegral(x + 0.1f, pitch, length) >= pulseIntegral(x, pitch, length) - 1e-5f);
        }
    }

    /*
    * The box-filtered cover equals a brute-force average of the hard slits over the footprint, near and far; a point
    * footprint samples the slit itself; a footprint of whole cells sees exactly the slit's share of a cell.
    */
    void testPulseCoverage() {
        const float pitch = 6.0f;
        const float length = 2.4f;
        assert(pulseCoverage(3.0f, 0.0f, pitch, length) == 1.0f && pulseCoverage(0.5f, 0.0f, pitch, length) == 0.0f);
        assert(near(pulseCoverage(17.3f, 3.0f * pitch, pitch, length), length / pitch, 1e-5f));
        assert(near(pulseCoverage(-200.0f, 500.0f, pitch, length), length / pitch, 2e-3f));

        for (int sample = 0; sample < 60; ++sample) {
            const float centre = -50.0f + 1.731f * static_cast<float>(sample);
            const float footprint = 0.05f + 0.37f * static_cast<float>(sample % 20);
            const int steps = 4000;
            float covered = 0.0f;

            for (int step = 0; step < steps; ++step) {
                const float x = centre - 0.5f * footprint + footprint * (static_cast<float>(step) + 0.5f) / static_cast<float>(steps);
                covered += pulseCoverage(x, 0.0f, pitch, length);
            }

            assert(near(pulseCoverage(centre, footprint, pitch, length), covered / static_cast<float>(steps), 3e-3f));
        }
    }

    /*
    * A face's seed: the same for numbers a rounding hair apart (interpolating three equal corners), different for
    * different faces; a cell's numbers the same all over it, 0..1, different in the next column or row, negative places
    * too; and over many cells about as many under a share as the share.
    */
    void testSeedsAndHashes() {
        const Vec2 numbers{0.3712f, 0.8044f};
        assert(windowFaceSeed(numbers) == windowFaceSeed(Vec2{0.3712f + 1e-6f, 0.8044f - 1e-6f}));
        assert(windowFaceSeed(numbers) != windowFaceSeed(Vec2{0.5f, 0.8044f}) && windowFaceSeed(numbers) != windowFaceSeed(Vec2{0.3712f, 0.2f}));

        const std::uint32_t seed = windowFaceSeed(numbers);
        const Vec3 cell = windowHash(13.2f, 40.3f, 6.0f, seed);
        assert(same(cell, windowHash(17.9f, 40.9f, 6.0f, seed)));
        assert(!same(cell, windowHash(18.1f, 40.3f, 6.0f, seed)) && !same(cell, windowHash(13.2f, 41.1f, 6.0f, seed)));
        assert(!same(cell, windowHash(13.2f, 40.3f, 6.0f, seed + 1)));
        assert(!same(windowHash(-3.0f, -2.5f, 6.0f, seed), windowHash(3.0f, 2.5f, 6.0f, seed)));

        int under = 0;
        const int cells = 4000;

        for (int index = 0; index < cells; ++index) {
            const Vec3 hashed = windowHash(6.0f * static_cast<float>(index % 80), static_cast<float>(index / 80), 6.0f, seed);
            assert(hashed.x >= 0.0f && hashed.x < 1.0f && hashed.y >= 0.0f && hashed.y < 1.0f && hashed.z >= 0.0f && hashed.z < 1.0f);
            under += hashed.x < 0.3f ? 1 : 0;
        }

        assert(near(static_cast<float>(under) / static_cast<float>(cells), 0.3f, 0.03f));
    }

    // The far blend: none while a pixel spans well under a cell (across or up), all from a cell and a half, never falling.
    void testFarShare() {
        assert(windowFarShare(Vec2{0.1f, 0.1f}, 6.0f) == 0.0f);
        assert(windowFarShare(Vec2{9.0f, 0.1f}, 6.0f) == 1.0f && windowFarShare(Vec2{0.1f, 1.5f}, 6.0f) == 1.0f);
        float previous = 0.0f;

        for (float footprint = 0.0f; footprint < 12.0f; footprint += 0.1f) {
            const float share = windowFarShare(Vec2{footprint, 0.0f}, 6.0f);
            assert(share >= previous);
            previous = share;
        }
    }

    /*
    * The emission: none outside a band; a near pixel in a lit slit shows its colour at 0.6-1.4 times; far, the band's
    * expected light (cover x slit share x share lit x the colours' mix, brightness 1). And the near windows average to
    * the far value over a wall of many cells: the fade keeps the band's energy.
    */
    void testEmission() {
        const Vec3 warm{2.0f, 1.0f, 0.4f};
        const Vec3 cool{0.3f, 1.5f, 2.0f};
        const Vec4 band{1.0f, 0.4f, 0.25f, 0.5f};
        const float pitch = 6.0f;
        const std::uint32_t seed = windowFaceSeed(Vec2{0.21f, 0.66f});
        const Vec3 none = windowEmission(Vec2{3.0f, 10.0f}, Vec2{0.01f, 0.01f}, Vec4{0.0f, 0.4f, 0.25f, 0.5f}, pitch, seed, warm, cool);
        assert(none.x == 0.0f && none.y == 0.0f && none.z == 0.0f);

        bool sawLit = false;

        for (int column = 0; column < 40 && !sawLit; ++column) {
            const Vec2 place{pitch * (static_cast<float>(column) + 0.5f), 10.5f};
            const Vec3 light = windowEmission(place, Vec2{0.01f, 0.01f}, band, pitch, seed, warm, cool);

            if (light.x > 0.0f) {
                const bool isWarm = near(light.y / light.x, warm.y / warm.x, 1e-4f);
                const bool isCool = near(light.y / light.x, cool.y / cool.x, 1e-4f);
                const float scale = isWarm ? light.x / warm.x : light.x / cool.x;
                assert((isWarm || isCool) && scale >= 0.6f - 1e-5f && scale <= 1.4f + 1e-5f);
                sawLit = true;
            }
        }

        assert(sawLit);

        const Vec3 average = warm + (cool - warm) * band.z;
        const Vec3 far = windowEmission(Vec2{1234.5f, 300.2f}, Vec2{60.0f, 10.0f}, band, pitch, seed, warm, cool);
        const Vec3 expected = average * (band.x * band.w * band.y);
        assert(near(far.x, expected.x, 1e-3f) && near(far.y, expected.y, 1e-3f) && near(far.z, expected.z, 1e-3f));

        Vec3 sum{0.0f, 0.0f, 0.0f};
        int samples = 0;

        for (int row = 0; row < 60; ++row) {
            for (int step = 0; step < 600; ++step) {
                const Vec2 place{0.1f * (static_cast<float>(step) + 0.5f), static_cast<float>(row) + 0.5f};
                sum = sum + windowEmission(place, Vec2{0.01f, 0.01f}, band, pitch, seed, warm, cool);
                ++samples;
            }
        }

        const Vec3 nearAverage = sum * (1.0f / static_cast<float>(samples));
        assert(near(nearAverage.x, expected.x, 0.12f * expected.x) && near(nearAverage.z, expected.z, 0.12f * expected.z));
    }
}

void testWindowMask() {
    testPulseIntegral();
    testPulseCoverage();
    testSeedsAndHashes();
    testFarShare();
    testEmission();
}
