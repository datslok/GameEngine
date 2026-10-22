#include "render/gpu/taa.h"
#include "render/gpu/column_major.h"
#include "render/gpu/bloom.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void visit(TaaSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Temporal anti-aliasing", settings.enabled);
    visitor.field("New frame's weight", settings.currentWeight, {.min = 0.01f, .max = 1.0f, .sliderMax = 0.5f});
    visitor.field("History clip width", settings.clipSigma, {.min = 0.0f, .max = 8.0f, .sliderMax = 3.0f, .unit = "sd"});
    visitor.field("Jitter", settings.jitter);
    visitor.field("Show motion", settings.showMotion);
}

// The comparisons are written so not a number fails them too.
void validateTaa(const TaaSettings& settings) {
    if (!(settings.currentWeight > 0.0f && settings.currentWeight <= 1.0f)) {
        throw std::invalid_argument("Temporal anti-aliasing needs a new frame's weight above 0 and at most 1");
    }

    if (!(settings.clipSigma >= 0.0f)) {
        throw std::invalid_argument("Temporal anti-aliasing's history clip width must not be negative");
    }
}

/*
* Each digit of the index, lowest first, goes one place further behind the point: in base 2, 1 is 0.1 (a half), 2 is
* 0.01 (a quarter), 3 is 0.11 (three quarters). Successive points always fill the biggest gap left, so any run of them
* covers the interval evenly (a low-discrepancy sequence), far more evenly than random points.
*/
float halton(std::uint32_t index, std::uint32_t base) {
    float result = 0.0f;
    float fraction = 1.0f;

    while (index > 0) {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }

    return result;
}

// Bases 2 and 3 for x and y: two different primes, so the points do not line up along a diagonal.
Vec2 taaJitter(std::uint64_t frame) {
    const std::uint32_t index = static_cast<std::uint32_t>(frame % taaJitterFrames) + 1;
    return Vec2{halton(index, 2) - 0.5f, halton(index, 3) - 0.5f};
}

/*
* Adding offset times w to clip x (and y) adds exactly the offset to x / w after the division, whatever w is. A pixel
* is 2 / width of NDC across, and pixels count down the screen while NDC y counts up, hence the minus.
*/
Mat4 jitteredClip(const Mat4& viewProjection, Vec2 pixelOffset, float width, float height) {
    const float offsetX = 2.0f * pixelOffset.x / width;
    const float offsetY = -2.0f * pixelOffset.y / height;
    Mat4 jittered = viewProjection;

    for (int column = 0; column < 4; ++column) {
        jittered.values[0][column] += offsetX * viewProjection.values[3][column];
        jittered.values[1][column] += offsetY * viewProjection.values[3][column];
    }

    return jittered;
}

Vec2 ndcToPixel(Vec2 ndc, Vec2 size) {
    return Vec2{(ndc.x + 1.0f) * 0.5f * size.x, (1.0f - ndc.y) * 0.5f * size.y};
}

Vec2 pixelToNdc(Vec2 pixel, Vec2 size) {
    return Vec2{pixel.x / size.x * 2.0f - 1.0f, 1.0f - pixel.y / size.y * 2.0f};
}

/*
* A point r measured from this frame's camera is r + (current - previous) measured from last frame's: one small
* translation (the step the camera took), so the numbers stay as small as camera-relative rendering keeps them.
*/
Mat4 previousRelativeViewProjection(const Mat4& previous, const Vec3& previousOrigin, const Vec3& currentOrigin) {
    const Vec3 step = currentOrigin - previousOrigin;
    return previous * Mat4::translation(step.x, step.y, step.z);
}

Mat4 taaReprojection(const Mat4& current, const Mat4& previous) {
    if (current == previous) {
        return Mat4::identity();
    }

    const std::optional<Mat4> undo = inverse(current);
    return undo ? previous * *undo : Mat4::identity();
}

/*
* The pixel's NDC and its depth are a point of this frame's clip space with w = 1: the reprojection takes it to last
* frame's clip space, and the division by w to last frame's screen. A w at or below zero is a point at or behind last
* frame's camera, which it could not see.
*/
Reprojected reprojectPixel(const Mat4& reprojection, Vec2 pixel, float depth, Vec2 size) {
    const Vec2 ndc = pixelToNdc(pixel, size);
    const Vec4 clip = reprojection * Vec4{ndc.x, ndc.y, depth, 1.0f};

    if (!(clip.w > 0.0f)) {
        return Reprojected{};
    }

    return Reprojected{ndcToPixel(Vec2{clip.x / clip.w, clip.y / clip.w}, size), true};
}

Vec2 previousPixelFromMotion(Vec2 pixel, Vec2 motion, Vec2 size) {
    return Vec2{pixel.x - motion.x * 0.5f * size.x, pixel.y + motion.y * 0.5f * size.y};
}

Vec2 objectMotion(const Vec4& currentClip, const Vec4& previousClip) {
    if (!(previousClip.w > 0.0f) || !(currentClip.w > 0.0f)) {
        const float none = static_cast<float>(TAA_NO_MOTION);
        return Vec2{none, none};
    }

    return Vec2{currentClip.x / currentClip.w - previousClip.x / previousClip.w, currentClip.y / currentClip.w - previousClip.y / previousClip.w};
}

Vec2 sampleOutputPixel(Vec2 renderPixel, Vec2 jitter, Vec2 outputPerRender) {
    return Vec2{(renderPixel.x - jitter.x) * outputPerRender.x, (renderPixel.y - jitter.y) * outputPerRender.y};
}

Vec2 renderPixelOf(Vec2 outputPixel, Vec2 renderPerOutput) {
    return Vec2{outputPixel.x * renderPerOutput.x, outputPixel.y * renderPerOutput.y};
}

/*
* Blackman-Harris is the window that keeps a picture sharp with the least ringing; this Gaussian is within a few
* hundredths of it over the 3x3 render pixels the resolve reads, and simpler. The squared distance saves a square root.
*/
float taaSampleWeight(float distanceSquared) {
    return std::exp(-static_cast<float>(TAA_SAMPLE_FALLOFF) * distanceSquared);
}

/*
* At the output's size a sample always lands within half a pixel's diagonal of the centre (a weight of 0.32 at worst,
* 0.8 on average), so the history averages a few frames longer than the setting alone says. Upscaled, a pixel between
* samples takes almost nothing new and keeps what earlier frames' samples, landing closer, gave it.
*/
float taaCurrentShare(float currentWeight, float nearestSampleWeight) {
    return currentWeight * nearestSampleWeight;
}

Vec3 toYCoCg(const Vec3& rgb) {
    return Vec3{0.25f * rgb.x + 0.5f * rgb.y + 0.25f * rgb.z, 0.5f * rgb.x - 0.5f * rgb.z, -0.25f * rgb.x + 0.5f * rgb.y - 0.25f * rgb.z};
}

Vec3 fromYCoCg(const Vec3& yCoCg) {
    return Vec3{yCoCg.x + yCoCg.y - yCoCg.z, yCoCg.x + yCoCg.z, yCoCg.x - yCoCg.y - yCoCg.z};
}

/*
* Measured in units of the box's half-widths, the history's offset from the centre is outside the box when its largest
* component passes 1; dividing the offset by that component brings it onto the box's surface, along the line to the
* centre (Playdead's clip, which keeps the history's hue better than clamping each component). A box with no width in
* some direction (every neighbour the same there) takes the history all the way to its centre.
*/
Vec3 clipToBox(const Vec3& history, const Vec3& centre, const Vec3& extent) {
    constexpr float thinnest = 1e-7f;
    const Vec3 offset = history - centre;
    const float reach = std::max({std::abs(offset.x) / std::max(extent.x, thinnest), std::abs(offset.y) / std::max(extent.y, thinnest),
                                  std::abs(offset.z) / std::max(extent.z, thinnest)});
    return reach > 1.0f ? centre + offset * (1.0f / reach) : history;
}

// The weights are bloom's Karis weights (bloom.h), which fight the same fireflies there.
Vec3 taaBlend(const Vec3& current, const Vec3& history, float currentWeight) {
    const float currentShare = currentWeight * karisWeight(current);
    const float historyShare = (1.0f - currentWeight) * karisWeight(history);
    return (current * currentShare + history * historyShare) * (1.0f / (currentShare + historyShare));
}

// Catmull-Rom's cubic with its tension at a half, written per texel and gathered by powers of t (Horner's rule).
Vec4 catmullRomWeights(float t) {
    return Vec4{t * (-0.5f + t * (1.0f - 0.5f * t)), 1.0f + t * t * (-2.5f + 1.5f * t), t * (0.5f + t * (2.0f - 1.5f * t)),
                t * t * (-0.5f + 0.5f * t)};
}

GpuTaaPass packTaaPass(const Mat4& reprojection, std::uint32_t width, std::uint32_t height, RenderSize render, Vec2 jitter,
                       const TaaSettings& settings, bool historyValid) {
    validateTaa(settings);

    GpuTaaPass pass{};
    writeColumnMajor(reprojection, pass.reprojection);
    pass.size[0] = static_cast<float>(width);
    pass.size[1] = static_cast<float>(height);
    pass.size[2] = 1.0f / static_cast<float>(width);
    pass.size[3] = 1.0f / static_cast<float>(height);
    pass.render[0] = static_cast<float>(render.width);
    pass.render[1] = static_cast<float>(render.height);
    pass.render[2] = jitter.x;
    pass.render[3] = jitter.y;
    pass.options[0] = settings.currentWeight;
    pass.options[1] = settings.clipSigma;
    pass.options[2] = historyValid ? 1.0f : 0.0f;
    pass.options[3] = settings.showMotion ? 1.0f : 0.0f;
    return pass;
}

GpuMotionView packMotionView(const Mat4& current, const Mat4& previous) {
    GpuMotionView view{};
    writeColumnMajor(current, view.current);
    writeColumnMajor(previous, view.previous);
    return view;
}
