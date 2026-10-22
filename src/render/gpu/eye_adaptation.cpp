#include "render/gpu/eye_adaptation.h"
#include "render/gpu/bloom_shared.h"

#include <algorithm>
#include <cmath>

float pixelLuminance(const Vec3& colour) {
    return static_cast<float>(BLOOM_LUMA_RED) * colour.x + static_cast<float>(BLOOM_LUMA_GREEN) * colour.y +
           static_cast<float>(BLOOM_LUMA_BLUE) * colour.z;
}

// The comparison is written so not a number fails it too.
int histogramBin(float luminance) {
    if (!(luminance > 0.0f)) {
        return 0;
    }

    const float share = (std::log2(luminance) - eyeHistogramLowestEv) / (eyeHistogramHighestEv - eyeHistogramLowestEv);
    const float bin = std::floor(std::clamp(share, 0.0f, 1.0f) * static_cast<float>(eyeHistogramBins));
    return std::min(static_cast<int>(bin), eyeHistogramBins - 1);
}

float histogramBinEv(int bin) {
    return eyeHistogramLowestEv + (static_cast<float>(bin) + 0.5f) / static_cast<float>(eyeHistogramBins) * (eyeHistogramHighestEv - eyeHistogramLowestEv);
}

/*
* Walking the bins darkest first, each holds the pixels from the running count to the running count plus its own; the
* part of that inside the span from low to high share of all pixels is its weight.
*/
float histogramMeanEv(std::span<const std::uint32_t> counts, float lowShare, float highShare, float fallbackEv) {
    float total = 0.0f;

    for (const std::uint32_t count : counts) {
        total += static_cast<float>(count);
    }

    const float spanStart = lowShare * total;
    const float spanEnd = highShare * total;
    float before = 0.0f;
    float weighted = 0.0f;
    float weight = 0.0f;

    for (std::size_t bin = 0; bin < counts.size(); ++bin) {
        const float after = before + static_cast<float>(counts[bin]);
        const float inside = std::max(std::min(after, spanEnd) - std::max(before, spanStart), 0.0f);
        weighted += inside * histogramBinEv(static_cast<int>(bin));
        weight += inside;
        before = after;
    }

    return weight > 0.0f ? weighted / weight : fallbackEv;
}

float targetExposureEv(float meanEv, const ToneMappingSettings& settings) {
    const float target = settings.adaptationStrength * (std::log2(settings.adaptationReference) - meanEv);
    return std::clamp(target, settings.lowestExposureEv, settings.highestExposureEv);
}

float adaptExposureEv(float currentEv, float targetEv, float seconds, const ToneMappingSettings& settings) {
    const float timeConstant = targetEv > currentEv ? settings.darkAdaptationSeconds : settings.brightAdaptationSeconds;

    if (timeConstant <= 0.0f) {
        return targetEv;
    }

    return currentEv + (targetEv - currentEv) * (1.0f - std::exp(-seconds / timeConstant));
}

/*
* The time adapting this frame is what the wait passes the delay by: none while still waiting, the whole frame once it
* has passed it, the part past it in the frame that crosses it.
*/
HeldExposure holdThenAdaptExposure(float currentEv, float targetEv, float heldSeconds, float seconds, const ToneMappingSettings& settings) {
    if (targetEv <= currentEv) {
        return HeldExposure{adaptExposureEv(currentEv, targetEv, seconds, settings), 0.0f};
    }

    const float held = heldSeconds + seconds;
    const float adapting = std::max(held - std::max(heldSeconds, settings.darkAdaptationDelaySeconds), 0.0f);
    return HeldExposure{adapting > 0.0f ? adaptExposureEv(currentEv, targetEv, adapting, settings) : currentEv, held};
}

GpuEyeAdaptationStep packEyeAdaptationStep(const ToneMappingSettings& settings, float seconds, bool snap) {
    return GpuEyeAdaptationStep{seconds,
                                settings.adaptationStrength,
                                std::log2(settings.adaptationReference),
                                settings.darkAdaptationSeconds,
                                settings.brightAdaptationSeconds,
                                settings.lowestExposureEv,
                                settings.highestExposureEv,
                                snap ? 1.0f : 0.0f,
                                settings.darkAdaptationDelaySeconds,
                                {}};
}

/*
* With no pixels to measure, the last frame's mean stands (or the reference before any): the exposure holds still. The
* adapted mean steps by holdThenAdaptExposure too, in negated stops: a darker scene is a higher exposure-like number,
* so into the dark it waits and eases at the dark time, into the light it eases at once at the bright one, as the
* exposure does.
*/
GpuEyeAdaptation adaptToHistogram(const GpuEyeAdaptation& previous, std::span<const std::uint32_t> counts, const ToneMappingSettings& settings,
                                  float seconds, bool snap) {
    const float fallback = previous.measured > 0.0f ? previous.meanEv : std::log2(settings.adaptationReference);
    const float meanEv = histogramMeanEv(counts, eyeLowShare, eyeHighShare, fallback);
    const float targetEv = targetExposureEv(meanEv, settings);
    const bool jump = snap || previous.measured <= 0.0f;
    const HeldExposure stepped = jump ? HeldExposure{targetEv, 0.0f}
                                      : holdThenAdaptExposure(previous.exposureEv, targetEv, previous.darkHeldSeconds, seconds, settings);
    const HeldExposure eye = jump ? HeldExposure{-meanEv, 0.0f}
                                  : holdThenAdaptExposure(-previous.adaptedMeanEv, -meanEv, previous.adaptedMeanHeldSeconds, seconds, settings);
    return GpuEyeAdaptation{stepped.exposureEv, targetEv, meanEv, 1.0f, stepped.heldSeconds, -eye.exposureEv, eye.heldSeconds, 0.0f};
}
