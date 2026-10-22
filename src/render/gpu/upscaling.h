#pragma once

#include "core/settings_visitor.h"

#include <cstdint>

/*
* Upscaling: the passes that cost per pixel (the depth prepass, the main pass, the velocity pass, the clusters) draw a
* smaller picture, the render size, and temporal anti-aliasing builds the output-sized one from it over several frames
* (TAAU, render/gpu/taa.h): the jitter moves the small grid by a different fraction of its pixel every frame, so over a
* cycle its samples land all over each output pixel. Everything after the resolve (eye adaptation, bloom, tone mapping,
* the debug lines and UI) works at the output size. The shadow atlas, light probes and sky do not depend on the screen,
* so they cost the same at any scale.
*/

// The smallest render scale the settings allow: below a third the history has too few samples per output pixel.
inline constexpr float minRenderScale = 0.33f;

struct UpscalingSettings {
    // The render size's share of the output's, across and down (0.5 draws a quarter of the pixels). Only while
    // temporal anti-aliasing is on, since it is what upscales.
    float renderScale = 1.0f;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(UpscalingSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a render scale outside minRenderScale..1.
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

/*
* The textures' mip bias, log2 of the render width over the output's: the GPU picks mips for the size of a render pixel,
* which at half size is two output pixels across, a level blurrier than the output deserves; the history averages the
* sharper level's extra detail over its samples instead.
*/
float textureLodBias(RenderSize render, std::uint32_t outputWidth);
