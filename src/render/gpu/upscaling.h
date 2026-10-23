#pragma once

#include "core/settings_visitor.h"
#include "math/mat4.h"
#include "math/vec2.h"
#include "math/vec3.h"

#include <array>
#include <cstdint>
#include <optional>

/*
* Upscaling: the passes that cost per pixel (the depth prepass, the main pass, the velocity pass, the clusters) draw a
* smaller picture, the render size, and temporal anti-aliasing builds the output-sized one from it over several frames
* (TAAU, render/gpu/taa.h): the jitter moves the small grid by a different fraction of its pixel every frame, so over a
* cycle its samples land all over each output pixel. Everything after the resolve (eye adaptation, bloom, tone mapping,
* the debug lines and UI) works at the output size. The shadow atlas, light probes and sky do not depend on the screen,
* so they cost the same at any scale.
*
* The upscaler may instead be AMD's FSR 3.1 (render/vulkan/vulkan_fsr.h), which takes the same jittered frames, depth
* and motion vectors and builds the output from them its own way, with its sharpening after, or NVIDIA's DLSS through
* Streamline (render/vulkan/vulkan_dlss.h), a trained network doing the same on RTX GPUs.
*/

// The smallest render scale the settings allow: below a third the history has too few samples per output pixel.
inline constexpr float minRenderScale = 0.33f;

/*
* The one anti-aliasing choice: which builds the output from the jittered frames, the engine's own temporal
* anti-aliasing, AMD's FSR 3.1 or NVIDIA's DLSS, or none (Off: no jitter, no motion vectors, no history). FSR and DLSS
* replace TAA's resolve; they still need its jitter and velocity pass.
*/
enum class Upscaler {
    Off,
    Taa,
    Fsr,
    Dlss
};

// In the order of Upscaler's values.
inline constexpr std::array<const char*, 4> upscalerNames{"Off", "TAA", "FSR 3.1", "DLSS"};

/*
* NVIDIA's DLSS models (Streamline's presets). Default lets DLSS pick per mode (the transformer model K for DLAA,
* Quality and Balanced, M for Performance, L for Ultra performance). NVIDIA's notes: J ghosts a little less than K but
* flickers more; L ghosts less than J and K, sharper and steadier but slower; M is like L, closer to J and K in speed.
* K trailed at the edge of the flashlight's beam (the light moves with the camera, so the surface's history carries the
* old edge), where FSR and TAA did not.
*/
enum class DlssModel {
    Default,
    J,
    K,
    L,
    M
};

// In the order of DlssModel's values.
inline constexpr std::array<const char*, 5> dlssModelNames{"Default", "J", "K", "L", "M"};

// Streamline's number for the model (sl::DLSSPreset): 0 for its default, 10 to 13 for J to M.
std::uint32_t dlssPresetNumber(DlssModel model);

// Which of the vendors' upscalers this PC can run (their libraries loaded, the GPU able).
struct UpscalerSupport {
    bool fsr = false;
    bool dlss = false;
};

struct UpscalingSettings {
    // FSR and DLSS only where they are available (activeUpscaler). DLSS by default at full size (DLAA; the user's
    // choice on Oct 23 2026), which falls back to temporal anti-aliasing without an RTX GPU or Streamline's DLLs.
    Upscaler upscaler = Upscaler::Dlss;

    // The render size's share of the output's, across and down (0.5 draws a quarter of the pixels). Only while
    // anti-aliasing is on, since it is what upscales.
    float renderScale = 1.0f;

    // FSR's sharpening after it upscales (RCAS, robust contrast-adaptive sharpening): 0 for none, up to 1. 0.8 by
    // default (the user's choice on Oct 23 2026; it was 0.5). DLSS and TAA do not read it.
    float sharpness = 0.8f;

    // The model DLSS runs, in every mode.
    DlssModel dlssModel = DlssModel::Default;
};

// Its fields, for the debug UI and the settings file (core/settings_visitor.h).
void visit(UpscalingSettings& settings, SettingsVisitor& visitor);

/*
* The named modes FSR and DLSS offer, each only a render scale: the output's size over the render's is 1, 1.5, 1.7, 2
* and 3 (so Quality draws 44% of the pixels, Performance a quarter). The debug UI's list sets the scale; the scale is
* still the one setting stored, and a scale between them is custom.
*/
enum class UpscalingMode {
    Native,
    Quality,
    Balanced,
    Performance,
    UltraPerformance
};

// In the order of UpscalingMode's values.
inline constexpr std::array<const char*, 5> upscalingModeNames{"Native", "Quality", "Balanced", "Performance", "Ultra performance"};

float upscalingModeScale(UpscalingMode mode);

// The mode a render scale is, within half a slider step (0.005, so 0.67 is Quality), or nothing for a custom scale.
std::optional<UpscalingMode> matchingUpscalingMode(float renderScale);

// The mode whose scale is nearest this one: DLSS works in its modes, so a custom scale draws in the nearest.
UpscalingMode nearestUpscalingMode(float renderScale);

// Throws std::invalid_argument for a render scale outside minRenderScale..1 or a sharpness outside 0..1.
void validateUpscaling(const UpscalingSettings& settings);

struct RenderSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    bool operator==(const RenderSize&) const = default;
};

// The output's size times the scale, each side rounded to the nearest pixel, at least one and at most the output's.
RenderSize renderSize(std::uint32_t outputWidth, std::uint32_t outputHeight, float scale);

// Whether the frame is smoothed over time (TAA, FSR or DLSS): jittered, with motion vectors and a history.
bool temporalAntiAliasing(const UpscalingSettings& settings);

// The scale the frame is drawn at: the setting's while anti-aliasing upscales, else 1.
float appliedRenderScale(const UpscalingSettings& settings);

// The upscaler in use: FSR or DLSS when chosen and available, else temporal anti-aliasing; Off when chosen.
Upscaler activeUpscaler(const UpscalingSettings& settings, UpscalerSupport support);

// A render size kept within a range, each side on its own (DLSS takes sizes from its mode's smallest to its largest).
RenderSize clampRenderSize(RenderSize size, RenderSize smallest, RenderSize largest);

/*
* The textures' mip bias, log2 of the render width over the output's: the GPU picks mips for the size of a render pixel,
* which at half size is two output pixels across, a level blurrier than the output deserves; the history averages the
* sharper level's extra detail over its samples instead. FSR and DLSS want one level more (AMD's and NVIDIA's
* log2(scale) - 1).
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

// A matrix the way Streamline stores one, DirectX's: row vectors, so the translation in the last row (ours transposed,
// the same 16 floats GLSL's column-major arrays get).
std::array<float, 16> streamlineMatrix(const Mat4& matrix);

// The camera as Streamline's constants name it: where it is and its axes in the world, its planes, lens and shape.
struct DlssCamera {
    Vec3 position;
    Vec3 up;
    Vec3 right;
    Vec3 forward;
    float cameraNear = 0.0f;
    float cameraFar = 0.0f;
    float verticalFov = 0.0f;
    float aspectRatio = 0.0f;
};

/*
* What DLSS is told each frame through Streamline besides its images, without its SDK's types
* (render/vulkan/vulkan_dlss.cpp copies them into its constants): the sizes, the jitter in render pixels, the scale
* turning the velocity image's motions (this frame's NDC minus last frame's) into Streamline's (the step from this
* frame's place to last frame's, in shares of the screen, y down), the lens and its inverse, the reprojection between
* the two frames' clip spaces (both camera-relative and unjittered) and its inverse, the camera, and whether the history
* must be dropped.
*/
struct DlssFrameInputs {
    RenderSize renderSize;
    std::uint32_t outputWidth = 0;
    std::uint32_t outputHeight = 0;
    Vec2 jitter;
    Vec2 motionVectorScale;
    std::array<float, 16> viewToClip{};
    std::array<float, 16> clipToView{};
    std::array<float, 16> clipToPrevClip{};
    std::array<float, 16> prevClipToClip{};
    DlssCamera camera;
    bool reset = false;
    std::uint32_t preset = 0; // the model, as Streamline numbers it (dlssPresetNumber)
};

DlssFrameInputs makeDlssFrameInputs(RenderSize render, std::uint32_t outputWidth, std::uint32_t outputHeight, Vec2 jitter, const Mat4& viewToClip,
                                    const Mat4& reprojection, const DlssCamera& camera, bool reset, DlssModel model);
