#include "engine/debug_views.h"
#include "render/gpu/eye_adaptation.h"
#include "render/gpu/tone_map_uniforms.h"
#include "render/gpu/tone_mapping.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
    // A histogram with these counts in these bins, the rest empty.
    std::vector<std::uint32_t> histogramOf(std::initializer_list<std::pair<int, std::uint32_t>> bins) {
        std::vector<std::uint32_t> counts(eyeHistogramBins, 0);

        for (const auto& [bin, count] : bins) {
            counts[static_cast<std::size_t>(bin)] += count;
        }

        return counts;
    }

    // White is 1; each primary counts as much as the eye weighs it.
    void testLuminance() {
        assert(near(pixelLuminance(Vec3{1.0f, 1.0f, 1.0f}), 1.0f, 1e-6f));
        assert(pixelLuminance(Vec3{0.0f, 1.0f, 0.0f}) > pixelLuminance(Vec3{1.0f, 0.0f, 0.0f}));
        assert(pixelLuminance(Vec3{1.0f, 0.0f, 0.0f}) > pixelLuminance(Vec3{0.0f, 0.0f, 1.0f}));
    }

    /*
    * Bins run evenly in stops: the lowest stop starts the first, the highest ends the last, nothing and too dark land in
    * the first, too bright in the last, and every bin's middle falls back into that bin.
    */
    void testBins() {
        assert(histogramBin(0.0f) == 0);
        assert(histogramBin(-1.0f) == 0);
        assert(histogramBin(std::numeric_limits<float>::quiet_NaN()) == 0);
        assert(histogramBin(std::exp2(eyeHistogramLowestEv - 3.0f)) == 0);
        assert(histogramBin(std::exp2(eyeHistogramHighestEv + 3.0f)) == eyeHistogramBins - 1);
        assert(histogramBin(std::numeric_limits<float>::infinity()) == eyeHistogramBins - 1);

        for (int bin = 0; bin < eyeHistogramBins; ++bin) {
            assert(histogramBin(std::exp2(histogramBinEv(bin))) == bin);
        }

        const float binWidth = (eyeHistogramHighestEv - eyeHistogramLowestEv) / static_cast<float>(eyeHistogramBins);
        assert(near(histogramBinEv(0), eyeHistogramLowestEv + 0.5f * binWidth, 1e-5f));
        assert(histogramBinEv(1) > histogramBinEv(0));
    }

    /*
    * The mean of the middle pixels: one bin alone gives that bin; the darkest half is left out (a black sky over half the
    * screen does not count), and so is the brightest tenth (a moon or a lamp); a bin straddling an edge counts only for
    * its part inside; nothing at all gives the fallback.
    */
    void testHistogramMean() {
        assert(near(histogramMeanEv(histogramOf({{100, 50}}), eyeLowShare, eyeHighShare, 0.0f), histogramBinEv(100), 1e-5f));

        const std::vector<std::uint32_t> nightSky = histogramOf({{10, 400}, {120, 580}, {250, 20}});
        assert(near(histogramMeanEv(nightSky, eyeLowShare, eyeHighShare, 0.0f), histogramBinEv(120), 1e-5f));

        // 30 of 100 pixels dark, 70 bright: from the 50th to the 90th all bright.
        assert(near(histogramMeanEv(histogramOf({{40, 30}, {140, 70}}), eyeLowShare, eyeHighShare, 0.0f), histogramBinEv(140), 1e-5f));

        // 60 dark and 40 bright over the whole span: the 50th to 60th dark, the 60th to 90th bright, so a quarter dark.
        const float straddling = histogramMeanEv(histogramOf({{40, 60}, {140, 40}}), eyeLowShare, eyeHighShare, 0.0f);
        assert(near(straddling, 0.25f * histogramBinEv(40) + 0.75f * histogramBinEv(140), 1e-4f));

        assert(histogramMeanEv(histogramOf({}), eyeLowShare, eyeHighShare, -2.5f) == -2.5f);
    }

    /*
    * The target: none at the reference, a darker scene raised by the strength's share of its stops, a brighter one
    * lowered, and both held within the limits.
    */
    void testTarget() {
        ToneMappingSettings settings;
        settings.lowestExposureEv = -8.0f;
        settings.highestExposureEv = 8.0f;
        const float referenceEv = std::log2(settings.adaptationReference);

        assert(near(targetExposureEv(referenceEv, settings), 0.0f, 1e-6f));
        assert(near(targetExposureEv(referenceEv - 5.0f, settings), 5.0f * settings.adaptationStrength, 1e-5f));
        assert(near(targetExposureEv(referenceEv + 2.0f, settings), -2.0f * settings.adaptationStrength, 1e-5f));
        assert(targetExposureEv(referenceEv - 40.0f, settings) == settings.highestExposureEv);
        assert(targetExposureEv(referenceEv + 40.0f, settings) == settings.lowestExposureEv);

        settings.adaptationStrength = 1.0f;
        assert(near(targetExposureEv(referenceEv - 3.0f, settings), 3.0f, 1e-5f));
        settings.adaptationStrength = 0.0f;
        assert(targetExposureEv(referenceEv - 3.0f, settings) == 0.0f);
    }

    /*
    * The step: after one time constant 1 - 1/e of the gap is closed; into the dark (the exposure rising) the slower
    * time, into the light the faster; no time, no change; a time constant of 0 jumps; and frames of any length add up
    * to the same, since the steps compound exponentially.
    */
    void testAdaptation() {
        ToneMappingSettings settings;
        const float darkStep = adaptExposureEv(0.0f, 4.0f, settings.darkAdaptationSeconds, settings);
        assert(near(darkStep, 4.0f * (1.0f - std::exp(-1.0f)), 1e-4f));

        const float brightStep = adaptExposureEv(4.0f, 0.0f, settings.brightAdaptationSeconds, settings);
        assert(near(brightStep, 4.0f * std::exp(-1.0f), 1e-4f));

        assert(adaptExposureEv(1.5f, 4.0f, 0.0f, settings) == 1.5f);
        assert(adaptExposureEv(0.0f, 4.0f, 0.5f, settings) < 4.0f - adaptExposureEv(4.0f, 0.0f, 0.5f, settings));

        float stepped = 0.0f;
        for (int frame = 0; frame < 60; ++frame) {
            stepped = adaptExposureEv(stepped, 3.0f, 1.0f / 60.0f, settings);
        }
        assert(near(stepped, adaptExposureEv(0.0f, 3.0f, 1.0f, settings), 1e-4f));

        settings.darkAdaptationSeconds = 0.0f;
        assert(adaptExposureEv(0.0f, 3.0f, 0.01f, settings) == 3.0f);
    }

    /*
    * Into the dark the eye first holds, as a real one stays nearly blind for a moment when a light goes out: the
    * exposure keeps still while the scene stays darker, until the delay has passed, then adapts as before, counting
    * only the time past the delay, so frames of any length add up to the same. A brighter frame resets the wait (a
    * flickering light never sets it off), and into the light there is no wait at all.
    */
    void testHoldThenAdapt() {
        ToneMappingSettings settings;
        const float delay = settings.darkAdaptationDelaySeconds;
        assert(delay > 0.0f);

        HeldExposure held = holdThenAdaptExposure(0.0f, 4.0f, 0.0f, delay * 0.5f, settings);
        assert(held.exposureEv == 0.0f && near(held.heldSeconds, delay * 0.5f, 1e-6f));

        held = holdThenAdaptExposure(held.exposureEv, 4.0f, held.heldSeconds, delay * 0.5f + 0.25f, settings);
        assert(near(held.exposureEv, adaptExposureEv(0.0f, 4.0f, 0.25f, settings), 1e-5f));

        HeldExposure stepped{0.0f, 0.0f};
        for (int frame = 0; frame < 120; ++frame) {
            stepped = holdThenAdaptExposure(stepped.exposureEv, 4.0f, stepped.heldSeconds, 1.0f / 60.0f, settings);
        }
        assert(near(stepped.exposureEv, adaptExposureEv(0.0f, 4.0f, 2.0f - delay, settings), 1e-4f));

        const HeldExposure brighter = holdThenAdaptExposure(1.0f, 0.0f, 0.7f, 0.1f, settings);
        assert(brighter.heldSeconds == 0.0f);
        assert(near(brighter.exposureEv, adaptExposureEv(1.0f, 0.0f, 0.1f, settings), 1e-6f));

        // A light flickering every 0.2 s keeps resetting the wait, so the exposure never starts rising.
        HeldExposure flicker{0.0f, 0.0f};
        for (int beat = 0; beat < 20; ++beat) {
            flicker = holdThenAdaptExposure(flicker.exposureEv, beat % 2 == 0 ? 4.0f : -0.01f, flicker.heldSeconds, 0.2f, settings);
            assert(flicker.exposureEv <= 0.0f);
        }

        settings.darkAdaptationDelaySeconds = 0.0f;
        assert(near(holdThenAdaptExposure(0.0f, 4.0f, 0.0f, 0.3f, settings).exposureEv, adaptExposureEv(0.0f, 4.0f, 0.3f, settings), 1e-6f));
    }

    // The whole frame's step: measured from the histogram, eased from the last frame, or snapped at once.
    void testFrameStep() {
        ToneMappingSettings settings;
        const std::vector<std::uint32_t> night = histogramOf({{80, 1000}});
        const float meanEv = histogramBinEv(80);
        const float target = targetExposureEv(meanEv, settings);

        const GpuEyeAdaptation snapped = adaptToHistogram(GpuEyeAdaptation{}, night, settings, 0.01f, true);
        assert(snapped.exposureEv == target && snapped.targetEv == target && near(snapped.meanEv, meanEv, 1e-6f) && snapped.measured == 1.0f);

        // Darker than the eye is set for: it holds, and its wait is kept for the next frame, which then adapts.
        const GpuEyeAdaptation held = adaptToHistogram(GpuEyeAdaptation{0.0f, 0.0f, 0.0f, 1.0f}, night, settings, 0.5f, false);
        const HeldExposure expectedHold = holdThenAdaptExposure(0.0f, target, 0.0f, 0.5f, settings);
        assert(held.exposureEv == expectedHold.exposureEv && held.darkHeldSeconds == expectedHold.heldSeconds);
        const GpuEyeAdaptation eased = adaptToHistogram(held, night, settings, 1.0f, false);
        assert(eased.exposureEv == holdThenAdaptExposure(held.exposureEv, target, held.darkHeldSeconds, 1.0f, settings).exposureEv);
        assert(eased.exposureEv > 0.0f);
        assert(snapped.darkHeldSeconds == 0.0f);

        // An empty histogram keeps the last measurement.
        const GpuEyeAdaptation empty = adaptToHistogram(snapped, histogramOf({}), settings, 0.5f, false);
        assert(near(empty.meanEv, snapped.meanEv, 1e-6f) && near(empty.exposureEv, snapped.exposureEv, 1e-6f));
    }

    // What the passes and tone mapping are told: the step's numbers, and whether the adapted exposure applies.
    void testPacking() {
        ToneMappingSettings settings;
        const GpuEyeAdaptationStep step = packEyeAdaptationStep(settings, 0.02f, true);
        assert(step.seconds == 0.02f && step.strength == settings.adaptationStrength && step.snap == 1.0f);
        assert(near(step.referenceEv, std::log2(settings.adaptationReference), 1e-6f));
        assert(step.darkSeconds == settings.darkAdaptationSeconds && step.brightSeconds == settings.brightAdaptationSeconds);
        assert(step.lowestEv == settings.lowestExposureEv && step.highestEv == settings.highestExposureEv);
        assert(step.darkDelaySeconds == settings.darkAdaptationDelaySeconds);
        assert(packEyeAdaptationStep(settings, 0.02f, false).snap == 0.0f);

        assert(packToneMapping(settings).output[3] == 1.0f);
        settings.eyeAdaptation = false;
        assert(packToneMapping(settings).output[3] == 0.0f);
    }

    // The readout: the exposure and the scene's mean in stops, signed, or that the exposure is fixed.
    void testReadout() {
        assert(exposureText(GpuEyeAdaptation{2.34f, 3.0f, -5.06f, 1.0f}) == "EXPOSURE +2.3 EV  SCENE -5.1 EV");
        assert(exposureText(GpuEyeAdaptation{-0.5f, 0.0f, 1.0f, 1.0f}) == "EXPOSURE -0.5 EV  SCENE +1.0 EV");
        assert(exposureText(std::nullopt) == "EXPOSURE FIXED");
    }

    // Settings that make no sense are refused: no reference brightness, a negative strength or time, limits crossed.
    void testValidation() {
        const auto refused = [](ToneMappingSettings settings) {
            try {
                validateToneMapping(settings, 1.0f);
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };

        assert(!refused(ToneMappingSettings{}));
        ToneMappingSettings bad;
        bad.adaptationReference = 0.0f;
        assert(refused(bad));
        bad = ToneMappingSettings{};
        bad.adaptationStrength = -0.1f;
        assert(refused(bad));
        bad = ToneMappingSettings{};
        bad.darkAdaptationSeconds = -1.0f;
        assert(refused(bad));
        bad = ToneMappingSettings{};
        bad.darkAdaptationDelaySeconds = -0.5f;
        assert(refused(bad));
        bad = ToneMappingSettings{};
        bad.lowestExposureEv = 3.0f;
        bad.highestExposureEv = 2.0f;
        assert(refused(bad));
    }
}

void testEyeAdaptation() {
    testLuminance();
    testBins();
    testHistogramMean();
    testTarget();
    testAdaptation();
    testHoldThenAdapt();
    testFrameStep();
    testPacking();
    testValidation();
    testReadout();
}
