#include "render/gpu/display_output.h"

#include <cassert>
#include <cmath>
#include <limits>

namespace {
    bool near(float actual, float expected) {
        return std::abs(actual - expected) < 1e-5f;
    }

    // A typical HDR monitor with Windows HDR on: 5 times SDR white at its peak, SDR white set to 2.5 (200 nits).
    DisplayCapabilities hdrMonitor() {
        DisplayCapabilities capabilities;
        capabilities.hdrSwapchainSupported = true;
        capabilities.hdrEnabled = true;
        capabilities.hdrHeadroom = 5.0f;
        capabilities.sdrWhiteLevel = 2.5f;
        return capabilities;
    }

    void assertIsSdr(const DisplayOutput& output) {
        assert(!output.hdr);
        assert(output.peakBrightness == sdrPeakBrightness);
        assert(output.whiteLevel == 1.0f);
    }

    // With HDR on and wanted, the curve reaches the monitor's headroom and white is shown at Windows' SDR white level,
    // so a white wall looks as bright as the desktop.
    void testHdrMonitorGetsHdr() {
        const DisplayOutput output = chooseDisplayOutput(DisplaySettings{}, hdrMonitor());

        assert(output.hdr);
        assert(output.peakBrightness == 5.0f);
        assert(output.whiteLevel == 2.5f);
    }

    // Any missing piece falls back to the ordinary screen: the player switched it off, the GPU cannot make an scRGB
    // swapchain, Windows HDR is off, or the monitor has no room above white.
    void testFallsBackToSdr() {
        DisplaySettings off;
        off.hdrOutput = false;
        assertIsSdr(chooseDisplayOutput(off, hdrMonitor()));

        DisplayCapabilities unsupported = hdrMonitor();
        unsupported.hdrSwapchainSupported = false;
        assertIsSdr(chooseDisplayOutput(DisplaySettings{}, unsupported));

        DisplayCapabilities windowsHdrOff = hdrMonitor();
        windowsHdrOff.hdrEnabled = false;
        assertIsSdr(chooseDisplayOutput(DisplaySettings{}, windowsHdrOff));

        DisplayCapabilities noHeadroom = hdrMonitor();
        noHeadroom.hdrHeadroom = 1.0f;
        assertIsSdr(chooseDisplayOutput(DisplaySettings{}, noHeadroom));

        assertIsSdr(chooseDisplayOutput(DisplaySettings{}, DisplayCapabilities{}));
    }

    // The player's paper white and peak, in nits, replace Windows' values. The monitor reaches 5 x 2.5 x 80 = 1000 nits.
    // White at 250 nits is 250 / 80 = 3.125 in scRGB, and a peak of 800 nits is 800 / 250 = 3.2 times that white.
    void testPlayerChoosesPaperWhiteAndPeak() {
        DisplaySettings settings;
        settings.paperWhiteNits = 250.0f;
        settings.peakNits = 800.0f;

        const DisplayOutput output = chooseDisplayOutput(settings, hdrMonitor());

        assert(output.hdr);
        assert(near(output.whiteLevel, 3.125f));
        assert(near(output.peakBrightness, 3.2f));

        // Only the peak set: white stays at Windows' 200 nits.
        DisplaySettings peakOnly;
        peakOnly.peakNits = 600.0f;
        const DisplayOutput windowsWhite = chooseDisplayOutput(peakOnly, hdrMonitor());
        assert(near(windowsWhite.whiteLevel, 2.5f));
        assert(near(windowsWhite.peakBrightness, 3.0f));
    }

    // The peak never goes past what the monitor can show, nor below white (white brighter than the peak would leave the
    // curve no room, so the peak rises to white instead).
    void testPeakStaysWithinTheMonitor() {
        DisplaySettings tooBright;
        tooBright.paperWhiteNits = 250.0f;
        tooBright.peakNits = 2000.0f;
        assert(near(chooseDisplayOutput(tooBright, hdrMonitor()).peakBrightness, 4.0f));

        DisplaySettings belowWhite;
        belowWhite.paperWhiteNits = 300.0f;
        belowWhite.peakNits = 200.0f;
        assert(chooseDisplayOutput(belowWhite, hdrMonitor()).peakBrightness == 1.0f);
    }

    // Nonsense from the system (not a number, or no white level) never reaches the shader: a bad headroom means SDR,
    // a bad white level means white at 1.
    void testNonsenseIsIgnored() {
        DisplayCapabilities badHeadroom = hdrMonitor();
        badHeadroom.hdrHeadroom = std::numeric_limits<float>::quiet_NaN();
        assertIsSdr(chooseDisplayOutput(DisplaySettings{}, badHeadroom));

        badHeadroom.hdrHeadroom = std::numeric_limits<float>::infinity();
        assertIsSdr(chooseDisplayOutput(DisplaySettings{}, badHeadroom));

        DisplayCapabilities badWhite = hdrMonitor();
        badWhite.sdrWhiteLevel = 0.0f;
        const DisplayOutput output = chooseDisplayOutput(DisplaySettings{}, badWhite);
        assert(output.hdr && output.whiteLevel == 1.0f);
    }
}

void testDisplayOutput() {
    testHdrMonitorGetsHdr();
    testFallsBackToSdr();
    testPlayerChoosesPaperWhiteAndPeak();
    testPeakStaysWithinTheMonitor();
    testNonsenseIsIgnored();
}
