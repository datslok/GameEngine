#include "render/gpu/eye_adaptation.h"
#include "render/gpu/tone_mapping.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <initializer_list>

namespace {
    constexpr ToneMapCurve allCurves[] = {ToneMapCurve::Clamp, ToneMapCurve::Reinhard, ToneMapCurve::Aces, ToneMapCurve::AgX};

    Vec3 grey(float value) {
        return Vec3{value, value, value};
    }

    // A grey through one curve, read from the red channel.
    float mapGrey(ToneMapCurve curve, float value, float exposure = 1.0f, float peak = sdrPeakBrightness) {
        return toneMap(grey(value), ToneMappingSettings{curve, exposure}, peak).x;
    }

    // Black stays exactly black, so the night sky is not lifted to grey.
    void testBlackStaysBlack() {
        for (ToneMapCurve curve : allCurves) {
            const Vec3 mapped = toneMap(Vec3{}, ToneMappingSettings{curve, 1.0f});
            assert(mapped.x == 0.0f && mapped.y == 0.0f && mapped.z == 0.0f);
        }
    }

    // Brighter in means at least as bright out, from deep shadow to far past the peak, and never past the peak:
    // swept in steps of a quarter stop from 1/65536 to 65536.
    void testBrighterNeverComesOutDarker() {
        for (ToneMapCurve curve : allCurves) {
            for (float peak : {sdrPeakBrightness, 4.0f}) {
                float previous = 0.0f;

                for (int quarterStop = -64; quarterStop <= 64; ++quarterStop) {
                    const float mapped = mapGrey(curve, std::exp2(static_cast<float>(quarterStop) / 4.0f), 1.0f, peak);

                    assert(mapped >= previous);
                    assert(mapped >= 0.0f && mapped <= peak);
                    previous = mapped;
                }
            }
        }
    }

    // Very bright light comes out at (nearly) the screen's peak: the curves fill the whole range.
    void testVeryBrightReachesThePeak() {
        for (ToneMapCurve curve : allCurves) {
            for (float peak : {sdrPeakBrightness, 4.0f}) {
                const float mapped = mapGrey(curve, 10000.0f * peak, 1.0f, peak);
                assert(mapped > 0.97f * peak && mapped <= peak);
            }
        }
    }

    // A mid grey (18% reflectance in full light, what a light meter aims for) stays a mid grey: every curve keeps it
    // between 10% and 30% of white, so switching curves changes the look but not the overall brightness much.
    void testMidGreyStaysMidGrey() {
        for (ToneMapCurve curve : allCurves) {
            const float mapped = mapGrey(curve, 0.18f);
            assert(mapped > 0.1f && mapped < 0.3f);
        }
    }

    // The curves work on each channel alike, so a grey stays grey: no curve tints white light.
    void testGreyStaysGrey() {
        for (ToneMapCurve curve : allCurves) {
            for (float value : {0.01f, 0.18f, 1.0f, 8.0f, 100.0f}) {
                const Vec3 mapped = toneMap(grey(value), ToneMappingSettings{curve, 1.0f});
                assert(std::abs(mapped.x - mapped.y) < 1e-3f * mapped.x + 1e-6f);
                assert(std::abs(mapped.x - mapped.z) < 1e-3f * mapped.x + 1e-6f);
            }
        }
    }

    // Clamp is the old look: unchanged up to the peak, cut off above it.
    void testClampCutsOffAtThePeak() {
        assert(mapGrey(ToneMapCurve::Clamp, 0.5f) == 0.5f);
        assert(mapGrey(ToneMapCurve::Clamp, 3.0f) == 1.0f);
        assert(mapGrey(ToneMapCurve::Clamp, 3.0f, 1.0f, 4.0f) == 3.0f);
    }

    // Reinhard is x / (1 + x): in deep shadow it barely changes anything (within 2% below 0.02), and the brightness that
    // equals the peak comes out at half of it.
    void testReinhardIsGentleInTheDark() {
        for (float value : {0.001f, 0.005f, 0.02f}) {
            assert(std::abs(mapGrey(ToneMapCurve::Reinhard, value) - value) < 0.02f * value);
        }

        assert(std::abs(mapGrey(ToneMapCurve::Reinhard, 1.0f) - 0.5f) < 1e-6f);
        assert(std::abs(mapGrey(ToneMapCurve::Reinhard, 4.0f, 1.0f, 4.0f) - 2.0f) < 1e-6f);
    }

    // Exposure scales the light before the curve, exactly like making the scene that much brighter.
    void testExposureScalesTheLightFirst() {
        for (ToneMapCurve curve : allCurves) {
            for (float value : {0.01f, 0.18f, 2.0f}) {
                assert(std::abs(mapGrey(curve, value, 2.0f) - mapGrey(curve, 2.0f * value)) < 1e-6f);
            }
        }
    }

    // A brighter screen gets the same curve stretched to its peak: peak * curve(x / peak). On an HDR screen four
    // times as bright as white, light four times as bright as white looks as it would at white on an ordinary one.
    void testHigherPeakStretchesTheCurve() {
        for (ToneMapCurve curve : allCurves) {
            for (float value : {0.05f, 0.5f, 6.0f}) {
                const float stretched = 4.0f * mapGrey(curve, value / 4.0f);
                assert(std::abs(mapGrey(curve, value, 1.0f, 4.0f) - stretched) < 1e-5f);
            }
        }
    }

    // Colours keep their order: of a red-orange light, red stays the brightest channel and blue the dimmest, at any
    // brightness, so a curve never changes which hue a light has.
    void testChannelOrderIsKept() {
        for (ToneMapCurve curve : allCurves) {
            for (float scale : {0.05f, 1.0f, 20.0f}) {
                const Vec3 mapped = toneMap(Vec3{1.0f, 0.4f, 0.1f} * scale, ToneMappingSettings{curve, 1.0f});
                assert(mapped.x >= mapped.y && mapped.y >= mapped.z);
            }
        }
    }

    /*
    * The rods' light: Larson's scotopic luminance, scaled so white keeps its brightness and growing with the light. Blue
    * counts for more than the cones weigh it, red for far less (the Purkinje shift), and black stays black.
    */
    void testRodLuminance() {
        assert(near(rodLuminance(grey(1.0f)), 1.0f, 1e-5f));
        assert(near(rodLuminance(grey(0.25f)), 0.25f, 1e-6f));
        assert(rodLuminance(Vec3{}) == 0.0f);

        const Vec3 blue{0.0f, 0.0f, 1.0f};
        const Vec3 red{1.0f, 0.0f, 0.0f};
        assert(rodLuminance(blue) > 2.0f * pixelLuminance(blue));
        assert(rodLuminance(red) < 0.25f * pixelLuminance(red));
        assert(near(rodLuminance(red * 4.0f), 4.0f * rodLuminance(red), 1e-6f));
    }

    /*
    * The rods' share: all of it while both the pixel and the scene the eye is set for are below the rods' threshold,
    * none once either is above the cones', smoothly less and less between (half way, half).
    */
    void testRodShare() {
        const ToneMappingSettings settings;
        const float dark = settings.rodsFullEv - 2.0f;
        const float bright = settings.conesFullEv + 1.0f;
        const float middle = 0.5f * (settings.rodsFullEv + settings.conesFullEv);

        assert(rodShare(dark, dark, settings) == 1.0f);
        assert(rodShare(bright, dark, settings) == 0.0f);
        assert(rodShare(dark, bright, settings) == 0.0f);
        assert(near(rodShare(middle, dark, settings), 0.5f, 1e-6f));
        assert(rodShare(middle - 0.5f, dark, settings) > rodShare(middle, dark, settings));
        assert(rodShare(middle, middle + 0.5f, settings) < rodShare(middle, dark, settings));
    }

    // A colour with this luminance's stops.
    Vec3 atStops(const Vec3& colour, float stops) {
        return colour * (std::exp2(stops) / pixelLuminance(colour));
    }

    /*
    * Night vision: in the dark a colour becomes the rods' grey (bluer with the blue shift, at the rods' brightness), in
    * the light it is left exactly as it was, and between it is a blend of the two.
    */
    void testNightVision() {
        ToneMappingSettings settings;
        const float darkEye = settings.rodsFullEv - 2.0f;
        const Vec3 warm{1.0f, 0.5f, 0.1f};

        settings.nightBlueShift = 0.0f;
        const Vec3 dim = atStops(warm, settings.rodsFullEv - 1.0f);
        const Vec3 rodGrey = nightVision(dim, darkEye, settings);
        assert(near(rodGrey.x, rodGrey.y, 1e-9f) && near(rodGrey.y, rodGrey.z, 1e-9f));
        assert(near(pixelLuminance(rodGrey), rodLuminance(dim), 1e-9f));

        settings.nightBlueShift = 1.0f;
        const Vec3 blued = nightVision(dim, darkEye, settings);
        assert(blued.z > blued.y && blued.y > blued.x);
        assert(near(pixelLuminance(blued), rodLuminance(dim), 1e-9f));

        // White keeps its brightness at any blue shift; black stays black.
        const Vec3 white = grey(std::exp2(settings.rodsFullEv - 1.0f));
        assert(near(pixelLuminance(nightVision(white, darkEye, settings)), pixelLuminance(white), 1e-9f));
        const Vec3 black = nightVision(Vec3{}, darkEye, settings);
        assert(black.x == 0.0f && black.y == 0.0f && black.z == 0.0f);

        // Bright enough, or an eye set for the light: untouched.
        const Vec3 lamp = atStops(warm, settings.conesFullEv + 1.0f);
        const Vec3 lampSeen = nightVision(lamp, darkEye, settings);
        assert(lampSeen.x == lamp.x && lampSeen.y == lamp.y && lampSeen.z == lamp.z);
        const Vec3 corner = nightVision(dim, settings.conesFullEv + 1.0f, settings);
        assert(corner.x == dim.x && corner.y == dim.y && corner.z == dim.z);

        // Between, the blend of the colour and the rods' grey by the rods' share.
        const Vec3 between = atStops(warm, 0.5f * (settings.rodsFullEv + settings.conesFullEv));
        const Vec3 seen = nightVision(between, darkEye, settings);
        const Vec3 rods = nightVision(between, darkEye, ToneMappingSettings{.rodsFullEv = 1.0f, .conesFullEv = 2.0f, .nightBlueShift = 1.0f});
        assert(near(seen.x, 0.5f * (between.x + rods.x), 1e-9f) && near(seen.z, 0.5f * (between.z + rods.z), 1e-9f));
    }
}

void testToneMapping() {
    testBlackStaysBlack();
    testBrighterNeverComesOutDarker();
    testVeryBrightReachesThePeak();
    testMidGreyStaysMidGrey();
    testGreyStaysGrey();
    testClampCutsOffAtThePeak();
    testReinhardIsGentleInTheDark();
    testExposureScalesTheLightFirst();
    testHigherPeakStretchesTheCurve();
    testChannelOrderIsKept();
    testRodLuminance();
    testRodShare();
    testNightVision();
}
