#pragma once

#include "math/vec3.h"
#include "render/gpu/eye_adaptation_shared.h"
#include "render/gpu/tone_mapping.h"

#include <array>
#include <cstdint>
#include <span>

/*
* Eye adaptation: an exposure that follows how bright the scene is, as an eye or a camera adapts between noon and
* moonlight. Each frame the scene's brightness is measured as a histogram of its pixels in stops (EV, log2 of
* luminance), the mean taken over its middle (the darkest half and the brightest tenth left out), and the exposure that
* would bring that mean to the reference brightness found, only partly (the strength), so a night stays darker than a
* day. The exposure eases towards it, slower into the dark than into the light, like an eye. It multiplies the tone
* mapping's own exposure, which becomes a bias. All in stops: each is a doubling.
*
* This is the API-free part, mirrored by assets/shaders/eye_adaptation.glsl and checked against it by the GPU tests.
*/

inline constexpr int eyeHistogramBins = EYE_HISTOGRAM_BINS;
inline constexpr float eyeHistogramLowestEv = static_cast<float>(EYE_HISTOGRAM_LOWEST_EV);
inline constexpr float eyeHistogramHighestEv = static_cast<float>(EYE_HISTOGRAM_HIGHEST_EV);
inline constexpr float eyeLowShare = static_cast<float>(EYE_LOW_SHARE);
inline constexpr float eyeHighShare = static_cast<float>(EYE_HIGH_SHARE);
inline constexpr std::uint32_t eyeHistogramGroupSide = EYE_HISTOGRAM_GROUP_SIDE;

// A pixel's luminance, as the eye weighs the three primaries (Rec. 709, the weights bloom uses).
float pixelLuminance(const Vec3& colour);

// The histogram bin a luminance falls into: its stops from the lowest, scaled to the bins, clamped to the first and the
// last; nothing (0, or not a number) in the first.
int histogramBin(float luminance);

// The stops a bin stands for: its middle.
float histogramBinEv(int bin);

/*
* The mean brightness in stops of the pixels between the low and the high share, darkest first: each bin counts only
* for the part of its pixels inside that span. With no pixels, the fallback.
*/
float histogramMeanEv(std::span<const std::uint32_t> counts, float lowShare, float highShare, float fallbackEv);

/*
* The exposure (stops) that brings a scene whose mean is meanEv towards the reference: strength times the difference
* (1 all the way, 0 not at all), kept between the lowest and the highest exposure.
*/
float targetExposureEv(float meanEv, const ToneMappingSettings& settings);

/*
* One frame's step towards the target: e^(-seconds / tau) of the gap is left, tau the dark adaptation's time when the
* exposure rises (the scene got darker) and the bright one's when it falls. A time of 0 reaches the target at once.
*/
float adaptExposureEv(float currentEv, float targetEv, float seconds, const ToneMappingSettings& settings);

// The exposure after a step and how long the scene has stayed darker than the eye is set for.
struct HeldExposure {
    float exposureEv = 0.0f;
    float heldSeconds = 0.0f;
};

/*
* One frame's step with the dark adaptation's wait: while the target lies above the exposure (the scene darker) the
* wait grows, and the exposure holds still until it passes the delay, then adapts by adaptExposureEv for only the time
* past the delay (so frames of any length add up to the same). The target at or below the exposure (the scene as
* bright or brighter) ends the wait and adapts at once: a flickering light never sets off the eye's adaptation to the
* dark. A real eye is all but blind for a moment after a light goes out, before it slowly sees again.
*/
HeldExposure holdThenAdaptExposure(float currentEv, float targetEv, float heldSeconds, float seconds, const ToneMappingSettings& settings);

// The adapted exposure the passes keep in GPU memory (std430): the exposure, the target and the scene's mean, in stops,
// and the dark adaptation's wait.
struct GpuEyeAdaptation {
    float exposureEv = 0.0f;
    float targetEv = 0.0f;
    float meanEv = 0.0f;
    float measured = 0.0f;        // 1 once a frame has been measured
    float darkHeldSeconds = 0.0f; // how long the scene has stayed darker than the eye is set for (holdThenAdaptExposure)
    float unused[3]{};
};

static_assert(sizeof(GpuEyeAdaptation) == 32, "GpuEyeAdaptation must match EyeAdaptation in eye_adaptation.glsl");

// The adaptation pass's push constants: this frame's time, the settings it needs, and whether to jump to the target.
struct GpuEyeAdaptationStep {
    float seconds = 0.0f;
    float strength = 0.0f;
    float referenceEv = 0.0f;
    float darkSeconds = 0.0f;
    float brightSeconds = 0.0f;
    float lowestEv = 0.0f;
    float highestEv = 0.0f;
    float snap = 0.0f;
    float darkDelaySeconds = 0.0f;
    float unused[3]{};
};

static_assert(sizeof(GpuEyeAdaptationStep) == 48, "GpuEyeAdaptationStep must match eye_adaptation.comp's push constants");

// snap: jump straight to the target (the first frame, golden images), since there is nothing to adapt from.
GpuEyeAdaptationStep packEyeAdaptationStep(const ToneMappingSettings& settings, float seconds, bool snap);

// The whole frame's step on the CPU, as the passes do it: the histogram's mean, the target, the step (or the snap).
GpuEyeAdaptation adaptToHistogram(const GpuEyeAdaptation& previous, std::span<const std::uint32_t> counts, const ToneMappingSettings& settings,
                                  float seconds, bool snap);
