#include "render/gpu/tone_map_uniforms.h"

/*
* An 8-bit screen needs the dither against banding; a float (HDR) screen image has no 256-level steps to hide.
*/
ToneMapUniformData packToneMapping(const ToneMappingSettings& settings, const DisplayOutput& display, const BloomComposite& bloom) {
    validateToneMapping(settings, display.peakBrightness);

    return ToneMapUniformData{
        {settings.exposure, display.peakBrightness, static_cast<float>(static_cast<int>(settings.curve)), bloom.strength},
        {display.whiteLevel, display.hdr ? 0.0f : 1.0f, bloom.scale, settings.eyeAdaptation ? 1.0f : 0.0f}
    };
}
