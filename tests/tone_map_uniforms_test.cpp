#include "render/gpu/tone_map_uniforms.h"

#include <cassert>
#include <stdexcept>

namespace {
    // The shader gets exposure, peak and the curve's number; an ordinary screen writes white as 1 and dithers.
    void testSdrPacking() {
        const ToneMapUniformData data = packToneMapping(ToneMappingSettings{ToneMapCurve::Aces, 1.5f});

        assert(data.parameters[0] == 1.5f);
        assert(data.parameters[1] == sdrPeakBrightness);
        assert(data.parameters[2] == static_cast<float>(TONE_MAP_ACES));
        assert(data.parameters[3] == 0.0f);
        assert(data.output[0] == 1.0f);
        assert(data.output[1] == 1.0f);
    }

    // An HDR screen gets its headroom as the peak and its white level as the output scale, and no dither: its float
    // image has no 256-level steps to hide.
    void testHdrPacking() {
        DisplayOutput display;
        display.hdr = true;
        display.peakBrightness = 5.0f;
        display.whiteLevel = 2.5f;

        const ToneMapUniformData data = packToneMapping(ToneMappingSettings{}, display);

        assert(data.parameters[1] == 5.0f);
        assert(data.output[0] == 2.5f);
        assert(data.output[1] == 0.0f);
    }

    // Bloom's strength and its 1 / level count ride in the spare slots; without bloom the strength is 0.
    void testBloomPacking() {
        const ToneMapUniformData none = packToneMapping(ToneMappingSettings{});
        assert(none.parameters[3] == 0.0f);

        const ToneMapUniformData data = packToneMapping(ToneMappingSettings{}, DisplayOutput{}, BloomComposite{0.04f, 0.25f});
        assert(data.parameters[3] == 0.04f);
        assert(data.output[2] == 0.25f);
    }

    // Nonsense settings are refused.
    void testNonsenseIsRefused() {
        bool refusedExposure = false;
        bool refusedPeak = false;

        try {
            packToneMapping(ToneMappingSettings{ToneMapCurve::AgX, -1.0f});
        }
        catch (const std::invalid_argument&) {
            refusedExposure = true;
        }

        try {
            DisplayOutput display;
            display.peakBrightness = 0.0f;
            packToneMapping(ToneMappingSettings{}, display);
        }
        catch (const std::invalid_argument&) {
            refusedPeak = true;
        }

        assert(refusedExposure && refusedPeak);
    }
}

void testToneMapUniforms() {
    testSdrPacking();
    testHdrPacking();
    testBloomPacking();
    testNonsenseIsRefused();
}
