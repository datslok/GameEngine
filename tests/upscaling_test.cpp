#include "render/gpu/upscaling.h"

#include "test_levels.h"

#include <cassert>
#include <cmath>
#include <stdexcept>

namespace {
    // The render size: the output's times the scale, rounded, never below one pixel nor above the output.
    void testRenderSize() {
        const RenderSize full = renderSize(2560, 1440, 1.0f);
        assert(full.width == 2560 && full.height == 1440);

        const RenderSize half = renderSize(2560, 1440, 0.5f);
        assert(half.width == 1280 && half.height == 720);

        // 1920 * 0.67 = 1286.4 and 1080 * 0.67 = 723.6: rounded to the nearest pixel.
        const RenderSize twoThirds = renderSize(1920, 1080, 0.67f);
        assert(twoThirds.width == 1286 && twoThirds.height == 724);

        const RenderSize tiny = renderSize(1, 2, 0.33f);
        assert(tiny.width == 1 && tiny.height == 1);
    }

    // The scale only applies while temporal anti-aliasing upscales; without it the frame is drawn at the output's size.
    void testAppliedScale() {
        UpscalingSettings settings;
        settings.renderScale = 0.5f;
        assert(appliedRenderScale(settings, true) == 0.5f);
        assert(appliedRenderScale(settings, false) == 1.0f);
        assert(appliedRenderScale(UpscalingSettings{}, true) == 1.0f);
    }

    /*
    * The textures' mip bias: none at full size, one level sharper at half size (a render pixel covers two output pixels
    * across, so the mips are chosen for the output's), from the sizes as rounded.
    */
    void testTextureLodBias() {
        assert(textureLodBias(RenderSize{2560, 1440}, 2560) == 0.0f);
        assert(near(textureLodBias(RenderSize{1280, 720}, 2560), -1.0f, 1e-6f));
        assert(near(textureLodBias(RenderSize{640, 360}, 2560), -2.0f, 1e-6f));
        assert(near(textureLodBias(renderSize(1920, 1080, 0.67f), 1920), std::log2(1286.0f / 1920.0f), 1e-6f));
    }

    // The defaults draw at full size; a scale outside its limits (or not a number) is refused.
    void testSettings() {
        const UpscalingSettings defaults;
        assert(defaults.renderScale == 1.0f);
        validateUpscaling(defaults);

        const auto refused = [](float scale) {
            UpscalingSettings settings;
            settings.renderScale = scale;

            try {
                validateUpscaling(settings);
            } catch (const std::invalid_argument&) {
                return true;
            }
            return false;
        };

        assert(!refused(minRenderScale) && !refused(0.5f));
        assert(refused(minRenderScale * 0.5f) && refused(1.5f) && refused(0.0f) && refused(std::nanf("")));
    }
}

void testUpscaling() {
    testRenderSize();
    testAppliedScale();
    testTextureLodBias();
    testSettings();
}
