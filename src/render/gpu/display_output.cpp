#include "render/gpu/display_output.h"

#include <algorithm>
#include <cmath>

namespace {
    bool isPositive(float value) {
        return std::isfinite(value) && value > 0.0f;
    }
}

/*
* Every condition has to hold, since showing a float image on a screen that cannot take it would look washed out or
* broken. A headroom of exactly 1 is an HDR mode with no room above white, which gains nothing.
*
* Brightness is worked out in scRGB units (80 nits each): the monitor's peak is its headroom times its white level.
* The player's nits are divided by 80 into the same units, and the tone mapper's peak is the chosen peak measured in
* units of the chosen white.
*/
DisplayOutput chooseDisplayOutput(const DisplaySettings& settings, const DisplayCapabilities& capabilities) {
    const bool usableHeadroom = std::isfinite(capabilities.hdrHeadroom) && capabilities.hdrHeadroom > 1.0f;

    if (!settings.hdrOutput || !capabilities.hdrSwapchainSupported || !capabilities.hdrEnabled || !usableHeadroom) {
        return DisplayOutput{};
    }

    const float systemWhite = isPositive(capabilities.sdrWhiteLevel) ? capabilities.sdrWhiteLevel : 1.0f;
    const float monitorPeak = capabilities.hdrHeadroom * systemWhite;

    const float white = isPositive(settings.paperWhiteNits) ? settings.paperWhiteNits / scrgbWhiteNits : systemWhite;
    const float peak = isPositive(settings.peakNits) ? std::min(settings.peakNits / scrgbWhiteNits, monitorPeak) : monitorPeak;

    DisplayOutput output;
    output.hdr = true;
    output.peakBrightness = std::max(peak / white, 1.0f);
    output.whiteLevel = white;
    return output;
}

// 10,000 nits is the most any HDR signal can describe (the PQ curve's top), so nothing brighter means anything.
void visit(DisplaySettings& settings, SettingsVisitor& visitor) {
    visitor.field("HDR output", settings.hdrOutput);
    visitor.field("Paper white", settings.paperWhiteNits, {.min = 0.0f, .max = 10000.0f, .sliderMax = 500.0f, .unit = "nits (0: Windows)"});
    visitor.field("Peak", settings.peakNits, {.min = 0.0f, .max = 10000.0f, .sliderMax = 2000.0f, .unit = "nits (0: monitor)"});
}
