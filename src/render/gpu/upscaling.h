#pragma once

#include "core/settings_visitor.h"
#include "math/vec2.h"

#include <array>
#include <cstdint>

/*
* Upscaling: the passes that cost per pixel (the depth prepass, the main pass, the velocity pass, the clusters) draw a
* smaller picture, the render size, and temporal anti-aliasing builds the output-sized one from it over several frames
* (TAAU, render/gpu/taa.h): the jitter moves the small grid by a different fraction of its pixel every frame, so over a
* cycle its samples land all over each output pixel. Everything after the resolve (eye adaptation, bloom, tone mapping,
* the debug lines and UI) works at the output size. The shadow atlas, light probes and sky do not depend on the screen,
* so they cost the same at any scale.
*
* The upscaler may instead be AMD's FSR 3.1 (render/vulkan/vulkan_fsr.h), which takes the same jittered frames, depth
* and motion vectors and builds the output from them its own way, with its sharpening after.
*/

// The smallest render scale the settings allow: below a third the history has too few samples per output pixel.
inline constexpr float minRenderScale = 0.33f;

// Which builds the output from the jittered frames: the engine's own temporal anti-aliasing, or AMD's FSR 3.1.
enum class Upscaler {
    Taa,
    Fsr
};

// In the order of Upscaler's values.
inline constexpr std::array<const char*, 2> upscalerNames{"TAA", "FSR 3.1"};

struct UpscalingSettings {
    // FSR only where its DLL loaded (activeUpscaler); either needs temporal anti-aliasing on, since its jitter and
    // velocity pass feed both.
    Upscaler upscaler = Upscaler::Taa;

    // The render size's share of the output's, across and down (0.5 draws a quarter of the pixels). Only while
    // temporal anti-aliasing is on, since it is what upscales.
    float renderScale = 1.0f;

    // FSR's sharpening after it upscales (RCAS, robust contrast-adaptive sharpening): 0 for none, up to 1.
    float sharpness = 0.5f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(UpscalingSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a render scale outside minRenderScale..1 or a sharpness outside 0..1.
void validateUpscaling(const UpscalingSettings& settings);

struct RenderSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const RenderSize&) const = default;
};

// The output's size times the scale, each side rounded to the nearest pixel, at least one and at most the output's.
RenderSize renderSize(std::uint32_t outputWidth, std::uint32_t outputHeight, float scale);

// The scale the frame is drawn at: the setting's while temporal anti-aliasing upscales, else 1.
float appliedRenderScale(const UpscalingSettings& settings, bool temporalAntiAliasing);

// The upscaler in use: FSR when chosen and its DLL loaded, else temporal anti-aliasing.
Upscaler activeUpscaler(const UpscalingSettings& settings, bool fsrAvailable);

/*
* The textures' mip bias, log2 of the render width over the output's: the GPU picks mips for the size of a render pixel,
* which at half size is two output pixels across, a level blurrier than the output deserves; the history averages the
* sharper level's extra detail over its samples instead. FSR wants one level more (AMD's log2(scale) - 1).
*/
float textureLodBias(RenderSize render, std::uint32_t outputWidth, Upscaler upscaler = Upscaler::Taa);

/*
* The jitter's cycle for FSR: ceil(8 n^2) frames, n the output's width over the render's. Each output pixel gets n^2
* times fewer samples a frame than at full size, so the cycle grows by as much to spread them as evenly over it.
*/
std::uint32_t jitterPhaseCount(RenderSize render, std::uint32_t outputWidth);

/*
* What FSR is told each frame besides its images, without its SDK's types (render/vulkan/vulkan_fsr.cpp copies them
* into its dispatch): the sizes, the jitter in render pixels, the scale turning the velocity image's motions (this
* frame's NDC minus last frame's) into FSR's (render pixels from this frame's place to last frame's), the frame time in
* milliseconds, the sharpening, the camera's lens (its planes swapped, as FSR names them for reversed-Z), and whether
* the history must be dropped.
*/
struct FsrFrameInputs {
    RenderSize renderSize;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    Vec2 jitter;
    Vec2 motionVectorScale;
    float frameMilliseconds = 0.0f;
    bool sharpen = false;
    float sharpness = 0.0f;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float verticalFov = 0.0f;
    bool reset = false;
};

FsrFrameInputs makeFsrFrameInputs(RenderSize render, std::uint32_t outputWidth, std::uint32_t outputHeight, Vec2 jitter, float frameSeconds,
                                  float cameraNear, float cameraFar, float verticalFov, bool reset, const UpscalingSettings& settings);
