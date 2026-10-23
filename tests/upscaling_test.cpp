#include "engine/debug_views.h"
#include "math/mat4.h"
#include "render/gpu/taa.h"
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

    // FSR's mips are one level sharper again than the scale alone gives, as AMD recommends (log2(scale) - 1).
    void testFsrLodBias() {
        assert(textureLodBias(RenderSize{2560, 1440}, 2560, Upscaler::Fsr) == -1.0f);
        assert(near(textureLodBias(RenderSize{1280, 720}, 2560, Upscaler::Fsr), -2.0f, 1e-6f));
        assert(textureLodBias(RenderSize{2560, 1440}, 2560, Upscaler::Taa) == 0.0f);
    }

    /*
    * The jitter's cycle for an upscaler: 8 phases at full size, times the scale's square fewer render pixels per
    * output pixel (ceil(8 n^2), n the output over the render width), so each output pixel still gets samples all over.
    */
    void testJitterPhases() {
        assert(jitterPhaseCount(RenderSize{2560, 1440}, 2560) == 8);
        assert(jitterPhaseCount(RenderSize{1280, 720}, 2560) == 32);
        assert(jitterPhaseCount(RenderSize{1707, 960}, 2560) == 18); // n = 1.4997: 8 n^2 = 17.99, rounded up
        assert(jitterPhaseCount(RenderSize{1706, 960}, 2560) == 19); // n = 1.5006: 8 n^2 = 18.01
        assert(jitterPhaseCount(RenderSize{960, 540}, 2880) == 72);
        assert(jitterPhaseCount(RenderSize{1, 1}, 1) == 8);
    }

    // FSR and DLSS are used only where they are available; asked for elsewhere, temporal anti-aliasing upscales instead.
    void testActiveUpscaler() {
        const UpscalerSupport both{.fsr = true, .dlss = true};
        UpscalingSettings settings;
        settings.upscaler = Upscaler::Taa;
        assert(activeUpscaler(settings, both) == Upscaler::Taa);
        settings.upscaler = Upscaler::Fsr;
        assert(activeUpscaler(settings, both) == Upscaler::Fsr);
        assert(activeUpscaler(settings, UpscalerSupport{.dlss = true}) == Upscaler::Taa);
        settings.upscaler = Upscaler::Dlss;
        assert(activeUpscaler(settings, both) == Upscaler::Dlss);
        assert(activeUpscaler(settings, UpscalerSupport{.fsr = true}) == Upscaler::Taa);
    }

    // DLSS's mips are a level sharper than the scale alone, as FSR's (NVIDIA's log2(scale) - 1).
    void testDlssLodBias() {
        assert(textureLodBias(RenderSize{2560, 1440}, 2560, Upscaler::Dlss) == -1.0f);
        assert(near(textureLodBias(RenderSize{1280, 720}, 2560, Upscaler::Dlss), -2.0f, 1e-6f));
    }

    /*
    * DLSS works in its named modes, each taking render sizes in a range round its own: a scale picks the mode nearest it
    * (0.75 is Quality, 0.4 Ultra performance), and the render size is kept inside the mode's range.
    */
    void testDlssModes() {
        assert(nearestUpscalingMode(1.0f) == UpscalingMode::Native);
        assert(nearestUpscalingMode(0.67f) == UpscalingMode::Quality);
        assert(nearestUpscalingMode(0.75f) == UpscalingMode::Quality);
        assert(nearestUpscalingMode(0.58f) == UpscalingMode::Balanced);
        assert(nearestUpscalingMode(0.5f) == UpscalingMode::Performance);
        assert(nearestUpscalingMode(0.4f) == UpscalingMode::UltraPerformance);
        assert(nearestUpscalingMode(0.33f) == UpscalingMode::UltraPerformance);
        assert(nearestUpscalingMode(0.95f) == UpscalingMode::Native);

        const RenderSize low{1280, 720};
        const RenderSize high{2560, 1440};
        assert((clampRenderSize(RenderSize{1707, 960}, low, high) == RenderSize{1707, 960}));
        assert((clampRenderSize(RenderSize{1000, 500}, low, high) == low));
        assert((clampRenderSize(RenderSize{845, 475}, RenderSize{853, 480}, RenderSize{853, 480}) == RenderSize{853, 480}));
        assert((clampRenderSize(RenderSize{3000, 600}, low, high) == RenderSize{2560, 720}));
    }

    /*
    * What DLSS is told each frame through Streamline, without its types. Its matrices are DirectX's way round (row
    * vectors, the translation in the last row): ours transposed, as GLSL's column-major arrays are. Its motion vectors
    * are normalised to the screen, from this frame's place to last frame's (UV, y down), so our NDC motions are scaled
    * by (-0.5, +0.5). The clip matrices invert each other, and a still camera's reprojection is the identity.
    */
    void testDlssFrameInputs() {
        const std::array<float, 16> moved = streamlineMatrix(Mat4::translation(1.0f, 2.0f, 3.0f));
        assert(moved[12] == 1.0f && moved[13] == 2.0f && moved[14] == 3.0f && moved[15] == 1.0f && moved[3] == 0.0f);

        const Mat4 lens = Mat4::perspective(1.0f, 16.0f / 9.0f, 0.1f, 500.0f);
        const Mat4 reprojection = Mat4::translation(0.01f, -0.02f, 0.0f);
        const DlssFrameInputs inputs = makeDlssFrameInputs(RenderSize{1280, 720}, 2560, 1440, Vec2{0.25f, -0.125f}, lens, reprojection,
                                                           DlssCamera{Vec3{1.0f, 2.0f, 3.0f}, Vec3{0.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f},
                                                                      Vec3{0.0f, 0.0f, -1.0f}, 0.1f, 500.0f, 1.0f, 16.0f / 9.0f},
                                                           true);

        const Vec2 previousNdc{-0.2f, 0.4f};
        const Vec2 currentNdc{0.1f, 0.3f};
        const auto uv = [](Vec2 ndc) { return Vec2{ndc.x * 0.5f + 0.5f, 0.5f - ndc.y * 0.5f}; };
        const Vec2 step = uv(previousNdc) - uv(currentNdc);
        const Vec2 motion = currentNdc - previousNdc;
        assert(near(motion.x * inputs.motionVectorScale.x, step.x, 1e-6f) && near(motion.y * inputs.motionVectorScale.y, step.y, 1e-6f));

        const auto product = [](const std::array<float, 16>& a, const std::array<float, 16>& b) {
            std::array<float, 16> result{};
            for (int row = 0; row < 4; ++row) {
                for (int column = 0; column < 4; ++column) {
                    for (int k = 0; k < 4; ++k) {
                        result[row * 4 + column] += a[row * 4 + k] * b[k * 4 + column];
                    }
                }
            }
            return result;
        };

        const std::array<float, 16> lensRoundTrip = product(inputs.viewToClip, inputs.clipToView);
        const std::array<float, 16> clipRoundTrip = product(inputs.clipToPrevClip, inputs.prevClipToClip);

        for (int index = 0; index < 16; ++index) {
            const float identity = index % 5 == 0 ? 1.0f : 0.0f;
            assert(near(lensRoundTrip[index], identity, 1e-4f) && near(clipRoundTrip[index], identity, 1e-5f));
        }

        assert(inputs.clipToPrevClip == streamlineMatrix(reprojection) && inputs.viewToClip == streamlineMatrix(lens));
        assert(inputs.jitter.x == 0.25f && inputs.jitter.y == -0.125f && inputs.reset);
        assert((inputs.renderSize == RenderSize{1280, 720}) && inputs.outputWidth == 2560 && inputs.outputHeight == 1440);
        assert(inputs.camera.position.z == 3.0f && inputs.camera.forward.z == -1.0f && inputs.camera.cameraNear == 0.1f);
        assert(inputs.camera.cameraFar == 500.0f && inputs.camera.verticalFov == 1.0f && near(inputs.camera.aspectRatio, 16.0f / 9.0f, 1e-6f));
    }

    /*
    * What FSR is told each frame. Its motion vectors run from this frame's pixel to last frame's, in render pixels
    * after its motionVectorScale; ours are this frame's NDC minus last frame's, so the scale turns a known motion into
    * exactly that step on the render screen (x right, y down). Frame time in milliseconds, sharpening from the setting,
    * the camera's lens.
    */
    void testFsrFrameInputs() {
        UpscalingSettings settings;
        settings.sharpness = 0.25f;
        const RenderSize render{1280, 720};
        const FsrFrameInputs inputs = makeFsrFrameInputs(render, 2560, 1440, Vec2{0.25f, -0.125f}, 1.0f / 60.0f, 0.1f, 500.0f, 1.0f, true, settings);

        const Vec2 size{1280.0f, 720.0f};
        const Vec2 previousNdc{-0.2f, 0.4f};
        const Vec2 currentNdc{0.1f, 0.3f};
        const Vec2 step = ndcToPixel(previousNdc, size) - ndcToPixel(currentNdc, size);
        const Vec2 motion = currentNdc - previousNdc;
        assert(near(motion.x * inputs.motionVectorScale.x, step.x, 1e-3f) && near(motion.y * inputs.motionVectorScale.y, step.y, 1e-3f));

        assert(inputs.renderSize == render && inputs.outputWidth == 2560 && inputs.outputHeight == 1440);
        assert(inputs.jitter.x == 0.25f && inputs.jitter.y == -0.125f);
        assert(near(inputs.frameMilliseconds, 16.6667f, 1e-3f));
        assert(inputs.sharpen && inputs.sharpness == 0.25f);
        // Reversed-Z: FSR wants the planes swapped, the far one as "near" (its own check warns otherwise).
        assert(inputs.cameraNear == 500.0f && inputs.cameraFar == 0.1f && inputs.verticalFov == 1.0f && inputs.reset);

        settings.sharpness = 0.0f;
        assert(!makeFsrFrameInputs(render, 2560, 1440, Vec2{}, 0.01f, 0.1f, 500.0f, 1.0f, false, settings).sharpen);
    }

    // The defaults are DLSS at full size (DLAA, the Native mode; temporal anti-aliasing where DLSS is missing) with FSR's
    // sharpness at 0.8; a scale or sharpness outside its limits (or not a number) is refused.
    void testSettings() {
        const UpscalingSettings defaults;
        assert(defaults.renderScale == 1.0f && defaults.upscaler == Upscaler::Dlss);
        assert(matchingUpscalingMode(defaults.renderScale) == UpscalingMode::Native);
        assert(defaults.sharpness == 0.8f);
        validateUpscaling(defaults);

        UpscalingSettings sharp;
        sharp.sharpness = 1.5f;
        bool sharpRefused = false;

        try {
            validateUpscaling(sharp);
        } catch (const std::invalid_argument&) {
            sharpRefused = true;
        }

        assert(sharpRefused);

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

namespace {
    /*
    * The named modes, as FSR and DLSS name them: each a fixed render scale (1, 1/1.5, 1/1.7, 1/2, 1/3), every one
    * within the slider's limits, and a scale reads back as its mode within half a slider step (0.67 is Quality), else
    * as none (custom).
    */
    void testModes() {
        assert(upscalingModeScale(UpscalingMode::Native) == 1.0f);
        assert(near(upscalingModeScale(UpscalingMode::Quality), 1.0f / 1.5f, 1e-6f));
        assert(near(upscalingModeScale(UpscalingMode::Balanced), 1.0f / 1.7f, 1e-6f));
        assert(upscalingModeScale(UpscalingMode::Performance) == 0.5f);
        assert(near(upscalingModeScale(UpscalingMode::UltraPerformance), 1.0f / 3.0f, 1e-6f));

        for (std::size_t index = 0; index < upscalingModeNames.size(); ++index) {
            const UpscalingMode mode = static_cast<UpscalingMode>(index);
            UpscalingSettings settings;
            settings.renderScale = upscalingModeScale(mode);
            validateUpscaling(settings);
            assert(matchingUpscalingMode(settings.renderScale) == mode);
        }

        assert(matchingUpscalingMode(0.67f) == UpscalingMode::Quality);
        assert(matchingUpscalingMode(0.59f) == UpscalingMode::Balanced);
        assert(!matchingUpscalingMode(0.75f).has_value());
        assert(!matchingUpscalingMode(0.62f).has_value());
    }

    // The F4 readout names the upscaler in use (FSR asked for but unavailable shows TAA) and the render scale.
    void testReadout() {
        assert(upscalerText(Upscaler::Fsr, 0.67f) == "FSR 3.1 AT 0.67");
        assert(upscalerText(Upscaler::Taa, 1.0f) == "TAA AT 1.00");
        assert(upscalerText(Upscaler::Dlss, 0.5f) == "DLSS AT 0.50");
    }
}

void testUpscaling() {
    testModes();
    testReadout();
    testRenderSize();
    testAppliedScale();
    testTextureLodBias();
    testFsrLodBias();
    testJitterPhases();
    testActiveUpscaler();
    testDlssLodBias();
    testDlssModes();
    testDlssFrameInputs();
    testFsrFrameInputs();
    testSettings();
}
