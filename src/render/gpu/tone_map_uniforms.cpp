#include "render/gpu/tone_map_uniforms.h"

#include <cmath>

/*
* An 8-bit screen needs the dither against banding; a float (HDR) screen image has no 256-level steps to hide.
*/
ToneMapUniformData packToneMapping(const ToneMappingSettings& settings, const DisplayOutput& display, const BloomComposite& bloom,
                                   const CameraSettings& camera, const CameraView& view) {
    validateToneMapping(settings, display.peakBrightness);
    validateCamera(camera);

    ToneMapUniformData data{
        {settings.exposure, display.peakBrightness, static_cast<float>(static_cast<int>(settings.curve)), bloom.strength},
        {display.whiteLevel, display.hdr ? 0.0f : 1.0f, bloom.scale, settings.eyeAdaptation ? 1.0f : 0.0f},
        {settings.rodsFullEv, settings.conesFullEv, settings.nightBlueShift, settings.nightVision ? 1.0f : 0.0f},
        {},
        {camera.contrast, camera.saturation, camera.lift, camera.gamma},
        {camera.gain, camera.vignetting, camera.chromaticAberration, gradesColour(camera) ? 1.0f : 0.0f},
        {},
    };

    const Mat4 balance = whiteBalanceMatrix(camera.whiteBalance, camera.tint);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            data.whiteBalance[row * 4 + column] = balance.values[row][column];
        }
    }

    const float tanHalfUp = std::tan(view.verticalFov * 0.5f);
    data.view[0] = tanHalfUp * view.aspectRatio;
    data.view[1] = tanHalfUp;
    data.view[2] = camera.filmGrain;
    data.view[3] = static_cast<float>(view.frame % cameraFrameCycle);
    return data;
}
