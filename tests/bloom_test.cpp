#include "render/gpu/bloom.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

namespace {
    bool near(float a, float b, float tolerance) {
        return std::fabs(a - b) <= tolerance;
    }

    bool nearVec(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    // The light an image holds: each texel's colour times the frame pixels it covers.
    float totalLight(const BloomImage& image, float pixelsPerTexel) {
        float total = 0.0f;

        for (const Vec3& pixel : image.pixels) {
            total += pixel.x;
        }

        return total * pixelsPerTexel;
    }

    bool throwsInvalid(const BloomSettings& settings) {
        try {
            validateBloom(settings);
        }
        catch (const std::invalid_argument&) {
            return true;
        }

        return false;
    }

    // Both filters only move light around: their weights sum to exactly 1.
    void testWeightsSumToOne() {
        assert(BLOOM_INNER_BOX_WEIGHT + 4.0 * BLOOM_OUTER_BOX_WEIGHT == 1.0);
        assert(4.0 * BLOOM_TENT_CORNER + 4.0 * BLOOM_TENT_EDGE + BLOOM_TENT_CENTRE == 1.0);
        assert(near(static_cast<float>(BLOOM_LUMA_RED + BLOOM_LUMA_GREEN + BLOOM_LUMA_BLUE), 1.0f, 1e-6f));
    }

    // The linear sampler: texel centres give the texel, halfway gives the average, past the edge repeats the edge.
    void testBilinearSampling() {
        BloomImage image(2, 1);
        image.at(1, 0) = Vec3{1.0f, 2.0f, 4.0f};

        assert(nearVec(sampleBilinear(image, 0.25f, 0.5f), Vec3{}, 1e-6f));
        assert(nearVec(sampleBilinear(image, 0.75f, 0.5f), Vec3{1.0f, 2.0f, 4.0f}, 1e-6f));
        assert(nearVec(sampleBilinear(image, 0.5f, 0.5f), Vec3{0.5f, 1.0f, 2.0f}, 1e-6f));
        assert(nearVec(sampleBilinear(image, 0.0f, 0.0f), Vec3{}, 1e-6f));
        assert(nearVec(sampleBilinear(image, 1.0f, 1.0f), Vec3{1.0f, 2.0f, 4.0f}, 1e-6f));
        assert(nearVec(sampleBilinear(image, -3.0f, 7.0f), Vec3{}, 1e-6f));
    }

    // Level 0 is half the frame; levels stop before the shorter side drops below 4 texels, and at the cap.
    void testLevelCounts() {
        assert(bloomLevelCount(1920, 1080, 6) == 6);
        assert(bloomLevelCount(1920, 1080, 8) == 8); // 540, 270, 135, 67, 33, 16, 8, 4
        assert(bloomLevelCount(3840, 2160, 8) == 8);
        assert(bloomLevelCount(16, 16, 8) == 2);     // 8, 4
        assert(bloomLevelCount(8, 8, 8) == 1);
        assert(bloomLevelCount(7, 7, 8) == 0);
        assert(bloomLevelCount(1, 1, 8) == 0);
        assert(bloomLevelCount(1000, 9, 8) == 1);    // the shorter side decides

        const BloomLevelSize first = bloomLevelSize(1920, 1080, 0);
        assert(first.width == 960 && first.height == 540);

        const BloomLevelSize third = bloomLevelSize(1920, 1080, 2);
        assert(third.width == 240 && third.height == 135);

        const BloomLevelSize tiny = bloomLevelSize(3, 3, 3);
        assert(tiny.width == 1 && tiny.height == 1);
    }

    // A flat image stays flat and keeps its brightness through every step, with or without the Karis average.
    void testFlatImageStaysFlat() {
        const Vec3 colour{0.3f, 1.2f, 5.0f};
        const BloomImage scene(37, 23, colour);

        for (bool karis : {false, true}) {
            const BloomImage half = bloomDownsample(scene, BloomLevelSize{18, 11}, karis);

            assert(half.width == 18 && half.height == 11);

            for (const Vec3& pixel : half.pixels) {
                assert(nearVec(pixel, colour, 1e-5f));
            }
        }

        BloomImage target(16, 10);
        bloomUpsampleAdd(target, BloomImage(8, 5, colour), 1.0f);

        for (const Vec3& pixel : target.pixels) {
            assert(nearVec(pixel, colour, 1e-5f));
        }

        // The whole chain: every level holds the colour, and dividing by their number gives it back.
        BloomSettings settings;
        settings.maxLevels = 4;
        const BloomImage blurred = bloomBlur(BloomImage(128, 96, colour), settings);

        assert(blurred.width == 64 && blurred.height == 48);

        for (const Vec3& pixel : blurred.pixels) {
            assert(nearVec(pixel, colour, 1e-4f));
        }
    }

    // A mirrored scene blurs to the mirrored result: the filters favour no direction.
    void testMirroredInMirroredOut() {
        BloomImage scene(32, 16);
        BloomImage mirrored(32, 16);

        for (std::uint32_t y = 0; y < 16; ++y) {
            for (std::uint32_t x = 0; x < 32; ++x) {
                const float value = static_cast<float>((x * 7 + y * 13) % 11) + (x == 5 && y == 9 ? 300.0f : 0.0f);
                scene.at(x, y) = Vec3{value, value * 0.5f, 1.0f};
                mirrored.at(31 - x, y) = scene.at(x, y);
            }
        }

        const BloomImage blurred = bloomBlur(scene, BloomSettings{});
        const BloomImage blurredMirror = bloomBlur(mirrored, BloomSettings{});

        for (std::uint32_t y = 0; y < blurred.height; ++y) {
            for (std::uint32_t x = 0; x < blurred.width; ++x) {
                assert(nearVec(blurred.at(x, y), blurredMirror.at(blurred.width - 1 - x, y), 1e-3f));
            }
        }
    }

    // One bright pixel far from the edges spreads out, fading with distance, but keeps all its light.
    void testBrightPixelKeepsItsLight() {
        BloomImage scene(256, 256);
        scene.at(128, 128) = Vec3{1000.0f, 1000.0f, 1000.0f};

        BloomSettings settings;
        settings.maxLevels = 4;
        settings.fireflyReduction = false;

        const BloomImage blurred = bloomBlur(scene, settings);

        // Each level 0 texel covers 2 x 2 frame pixels.
        assert(near(totalLight(blurred, 4.0f), 1000.0f, 1.0f));

        // Spread: the brightest texel holds only part of the light, and some reaches 16 texels away.
        assert(blurred.at(64, 64).x < 0.5f * 1000.0f / 4.0f);
        assert(blurred.at(80, 64).x > 0.0f);

        // Fading along the row from the centre.
        float previous = blurred.at(64, 64).x;

        for (std::uint32_t step : {2u, 4u, 8u, 16u, 32u}) {
            const float value = blurred.at(64 + step, 64).x;
            assert(value < previous);
            previous = value;
        }
    }

    // The Karis average keeps a lone extreme pixel's own texel but takes away most of its spread into the texels around
    // it (where it is outweighed by the dark boxes), so it cannot bloom into a blob; a broad bright area is unchanged.
    void testFireflyReduction() {
        BloomImage sparkle(64, 64);
        sparkle.at(32, 32) = Vec3{1000.0f, 1000.0f, 1000.0f};

        const BloomImage plain = bloomDownsample(sparkle, BloomLevelSize{32, 32}, false);
        const BloomImage reduced = bloomDownsample(sparkle, BloomLevelSize{32, 32}, true);

        assert(near(reduced.at(16, 16).x, plain.at(16, 16).x, 1e-3f));
        assert(reduced.at(15, 16).x < 0.1f * plain.at(15, 16).x);
        assert(totalLight(reduced, 4.0f) < 0.5f * totalLight(plain, 4.0f));

        // A 16 x 16 block: an output texel whose reads all land inside it keeps the block's colour exactly.
        BloomImage block(64, 64);

        for (std::uint32_t y = 24; y < 40; ++y) {
            for (std::uint32_t x = 24; x < 40; ++x) {
                block.at(x, y) = Vec3{1000.0f, 1000.0f, 1000.0f};
            }
        }

        const BloomImage blockHalf = bloomDownsample(block, BloomLevelSize{32, 32}, true);
        assert(nearVec(blockHalf.at(16, 16), Vec3{1000.0f, 1000.0f, 1000.0f}, 0.01f));

        assert(karisWeight(Vec3{}) == 1.0f);
        assert(near(karisWeight(Vec3{1.0f, 1.0f, 1.0f}), 0.5f, 1e-6f));
    }

    // A wider tent reaches further: from 2.75 texels away only the radius 2 tent picks up a bright texel.
    void testRadiusSpreadsFurther() {
        BloomImage smaller(8, 8);
        smaller.at(4, 4) = Vec3{1.0f, 1.0f, 1.0f};

        BloomImage tight(16, 16);
        BloomImage wide(16, 16);
        bloomUpsampleAdd(tight, smaller, 1.0f);
        bloomUpsampleAdd(wide, smaller, 2.0f);

        assert(tight.at(14, 9).x == 0.0f);
        assert(wide.at(14, 9).x > 0.0f);
    }

    // Strength 0 gives the scene back exactly, 1 gives the bloom, and in between it mixes.
    void testMix() {
        const Vec3 scene{0.2f, 0.4f, 0.6f};
        const Vec3 bloom{50.0f, 60.0f, 70.0f};

        const Vec3 none = mixBloom(scene, bloom, 0.0f);
        assert(none.x == scene.x && none.y == scene.y && none.z == scene.z);

        assert(nearVec(mixBloom(scene, bloom, 1.0f), bloom, 1e-6f));
        assert(nearVec(mixBloom(scene, bloom, 0.25f), Vec3{12.65f, 15.3f, 17.95f}, 1e-4f));
    }

    // The tone mapping pass mixes bloom only when it is on and the frame has levels, scaled by one over their number.
    void testComposite() {
        BloomSettings settings;
        const BloomComposite on = bloomComposite(settings, 6);
        assert(on.strength == settings.strength);
        assert(near(on.scale, 1.0f / 6.0f, 1e-7f));

        assert(bloomComposite(settings, 0).strength == 0.0f);

        settings.enabled = false;
        assert(bloomComposite(settings, 6).strength == 0.0f);
    }

    // Nonsense settings are refused, by bloomBlur too.
    void testValidation() {
        validateBloom(BloomSettings{});

        BloomSettings settings;
        settings.strength = -0.01f;
        assert(throwsInvalid(settings));

        settings = BloomSettings{};
        settings.strength = 1.01f;
        assert(throwsInvalid(settings));

        settings = BloomSettings{};
        settings.radius = 0.0f;
        assert(throwsInvalid(settings));

        settings = BloomSettings{};
        settings.maxLevels = 0;
        assert(throwsInvalid(settings));

        settings = BloomSettings{};
        settings.maxLevels = BLOOM_LEVEL_LIMIT + 1;
        assert(throwsInvalid(settings));

        bool blurRefused = false;

        try {
            bloomBlur(BloomImage(16, 16), settings);
        }
        catch (const std::invalid_argument&) {
            blurRefused = true;
        }

        assert(blurRefused);
    }

    // A frame too small for any level blurs to nothing.
    void testTinyFrameHasNoBloom() {
        const BloomImage blurred = bloomBlur(BloomImage(6, 6, Vec3{1.0f, 1.0f, 1.0f}), BloomSettings{});
        assert(blurred.width == 0 && blurred.height == 0 && blurred.pixels.empty());
    }

    // The shaders get both texel sizes and the pass's option.
    void testPassPacking() {
        const BloomPassUniformData data = packBloomPass(BloomLevelSize{960, 540}, BloomLevelSize{480, 270}, 1.0f);

        assert(data.texelSizes[0] == 1.0f / 960.0f);
        assert(data.texelSizes[1] == 1.0f / 540.0f);
        assert(data.texelSizes[2] == 1.0f / 480.0f);
        assert(data.texelSizes[3] == 1.0f / 270.0f);
        assert(data.options[0] == 1.0f);
    }
}

void testBloom() {
    testWeightsSumToOne();
    testBilinearSampling();
    testLevelCounts();
    testFlatImageStaysFlat();
    testMirroredInMirroredOut();
    testBrightPixelKeepsItsLight();
    testFireflyReduction();
    testRadiusSpreadsFurther();
    testMix();
    testComposite();
    testValidation();
    testTinyFrameHasNoBloom();
    testPassPacking();
}
