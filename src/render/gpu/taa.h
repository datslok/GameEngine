#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"
#include "math/vec4.h"
#include "render/gpu/taa_shared.h"
#include "render/gpu/upscaling.h"

#include <cstdint>

/*
* Temporal anti-aliasing (TAA). Each pixel shows the scene at one point, its centre, so detail finer than a pixel aliases:
* edges step, thin things break up and shimmer as the camera moves. TAA spreads the samples over time instead of
* taking many per frame: every frame the whole projection moves by a different fraction of a pixel (the jitter, a cycle
* of Halton points), and each pixel keeps a running average of what it saw (the history), about the last ten frames.
*
* When things move, the history at a pixel shows something else, so each pixel finds where its surface was last frame
* (reprojection): from its depth and the two frames' cameras for anything still, from the velocity image a small pass
* draws for anything that moved (its motion vector). History that no longer fits (a surface that was hidden, light that
* changed) is clipped into the range of colours the pixel's neighbours show this frame, and the blend weighs each sample
* down by its brightness, so one very bright sample cannot flash.
*
* It also upscales (render/gpu/upscaling.h): the current frame may be drawn smaller than the history, so each output
* pixel rebuilds it from the nearest render pixels, each weighed by how close its jittered sample landed, and takes less
* of the new frame where none landed close.
*
* This is the API-free part, mirrored by assets/shaders/taa.glsl and checked against it by the GPU tests.
*/

inline constexpr std::uint64_t taaJitterFrames = TAA_JITTER_FRAMES;

// A capture draws this many frames first (two jitter cycles), so a still view's history has settled.
inline constexpr int taaSettleFrames = 2 * TAA_JITTER_FRAMES;

struct TaaSettings {
    bool enabled = true;

    // The share of each new frame in the running average: 0.1 averages about the last ten frames. More follows changes
    // faster but smooths less.
    float currentWeight = 0.1f;

    // How wide the box of the neighbours' colours the history is clipped into is, in their standard deviations: wider
    // keeps more history (smoother, but ghosts behind moving things), narrower less (sharper changes, more flicker).
    float clipSigma = 1.0f;

    // Off holds the projection still, for comparing; the history then only smooths noise.
    bool jitter = true;

    // A debug view: each pixel's motion since last frame in place of the picture.
    bool showMotion = false;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(TaaSettings& settings, SettingsVisitor& visitor);

// Throws std::invalid_argument for a current weight outside (0, 1] or a negative clip width.
void validateTaa(const TaaSettings& settings);

// The radical inverse of index in base: its digits mirrored behind the point (base 2: 1 to 0.5, 2 to 0.25, 3 to 0.75).
float halton(std::uint32_t index, std::uint32_t base);

// This frame's jitter in pixels, each within half a pixel of the centre: the Halton (2, 3) point of the frame's place
// in a cycle of phases frames (counted from 1, since point 0 is the corner). FSR wants longer cycles when it upscales.
Vec2 taaJitter(std::uint64_t frame, std::uint32_t phases = taaJitterFrames);

// A view-projection whose picture lands pixelOffset pixels over (x right, y down the screen), on a screen of this size:
// clip x and y move by the offset times w, so every point on screen moves the same, near or far, and depth does not change.
Mat4 jitteredClip(const Mat4& viewProjection, Vec2 pixelOffset, float width, float height);

// Normalised device coordinates to pixels and back, for the flipped viewport: (-1, 1) is the top-left corner, y down the screen.
Vec2 ndcToPixel(Vec2 ndc, Vec2 size);
Vec2 pixelToNdc(Vec2 pixel, Vec2 size);

// Last frame's camera-relative view-projection made to take points measured from this frame's camera: it moves them by
// the step between the two cameras first.
Mat4 previousRelativeViewProjection(const Mat4& previous, const Vec3& previousOrigin, const Vec3& currentOrigin);

// The matrix from this frame's clip space to last frame's: previous times the inverse of current, exactly the identity
// when the two are the same (a still camera), so a still picture's history is read exactly where it was written.
Mat4 taaReprojection(const Mat4& current, const Mat4& previous);

// Where a pixel (its centre) and its depth were on last frame's screen, and whether they were in front of that camera.
struct Reprojected {
    Vec2 pixel;
    bool valid = false;
};

Reprojected reprojectPixel(const Mat4& reprojection, Vec2 pixel, float depth, Vec2 size);

/*
* The camera's motion at a pixel and its depth, as the velocity image holds a motion (this frame's NDC minus last
* frame's), for the pixels no moving object covered: none for a point at or behind last frame's camera. FSR needs it in
* every pixel; the engine's own resolve reprojects such pixels itself.
*/
Vec2 cameraMotion(const Mat4& reprojection, Vec2 pixel, float depth, Vec2 size);

// Where an object drawn into the velocity image covered last frame: its motion is this frame's NDC minus last frame's.
Vec2 previousPixelFromMotion(Vec2 pixel, Vec2 motion, Vec2 size);

// A moving object's motion at a pixel: this frame's NDC minus last frame's, from the two clip positions; no motion
// (TAA_NO_MOTION, so the camera's is used) when either was at or behind its camera.
Vec2 objectMotion(const Vec4& currentClip, const Vec4& previousClip);

/*
* Where a render pixel's sample lies on the output screen: the jitter moved the picture by jitter render pixels, so the
* point seen at renderPixel is the unjittered picture's point at renderPixel - jitter, then scaled to output pixels.
*/
Vec2 sampleOutputPixel(Vec2 renderPixel, Vec2 jitter, Vec2 outputPerRender);

// An output pixel's place on the render screen (both counted from the top-left corner).
Vec2 renderPixelOf(Vec2 outputPixel, Vec2 renderPerOutput);

// The reconstruction's weight for a sample at this squared distance in output pixels from the pixel's centre.
float taaSampleWeight(float distanceSquared);

// The new frame's share of the blend: the setting's times the nearest sample's weight, so a pixel no sample landed
// close to this frame leans on its history (which earlier frames' samples filled in).
float taaCurrentShare(float currentWeight, float nearestSampleWeight);

// Luminance and two colour differences (Y = R/4 + G/2 + B/4), and back. The neighbours' colours spread less in it than
// in RGB, so the box the history is clipped into fits them more tightly.
Vec3 toYCoCg(const Vec3& rgb);
Vec3 fromYCoCg(const Vec3& yCoCg);

// The history moved along the line to the box's centre until it lies in the box (centre plus or minus extent); left as
// it is when it already does.
Vec3 clipToBox(const Vec3& history, const Vec3& centre, const Vec3& extent);

// The running average's step: current weighs currentWeight and history the rest, each also times its Karis weight,
// 1 / (1 + its luminance) (Karis 2014, bloom.h), so a lone very bright sample does not outweigh many ordinary ones.
Vec3 taaBlend(const Vec3& current, const Vec3& history, float currentWeight);

// Catmull-Rom's weights for the four texels round a point t (0..1) of the way from the second to the third: a cubic
// that passes through the texels, so reading the history with it does not blur it a little more every frame.
Vec4 catmullRomWeights(float t);

// The resolve pass's push constants (taa.comp): the reprojection (column-major), the output screen (width, height and
// their inverses), the render screen with this frame's jitter, and the settings with whether the history may be used.
struct GpuTaaPass {
    float reprojection[16];
    float size[4];
    float render[4];  // xy: the render size in pixels, zw: this frame's jitter in render pixels
    float options[4]; // x: the current weight, y: the clip width, z: 1 when the history is valid, w: 1 to show the motion
};

static_assert(sizeof(GpuTaaPass) == 112, "GpuTaaPass must match taa.comp's push constants");

GpuTaaPass packTaaPass(const Mat4& reprojection, std::uint32_t width, std::uint32_t height, RenderSize render, Vec2 jitter,
                       const TaaSettings& settings, bool historyValid);

// The velocity pass's push constants (motion.vert): this frame's and last frame's camera-relative view-projections,
// without the jitter, both taking points measured from this frame's camera.
struct GpuMotionView {
    float current[16];
    float previous[16];
};

static_assert(sizeof(GpuMotionView) == 128, "GpuMotionView must fit the 128 bytes of push constants every GPU has");

GpuMotionView packMotionView(const Mat4& current, const Mat4& previous);

// The camera motion pass's push constants (motion_fill.comp): the reprojection (column-major) and the render screen it
// walks (width, height).
struct GpuMotionFillPass {
    float reprojection[16];
    float size[4];
};

static_assert(sizeof(GpuMotionFillPass) == 80, "GpuMotionFillPass must match motion_fill.comp's push constants");

GpuMotionFillPass packMotionFillPass(const Mat4& reprojection, RenderSize render);
