#ifndef EYE_ADAPTATION_GLSL
#define EYE_ADAPTATION_GLSL

// Eye adaptation (render/gpu/eye_adaptation.h, which explains each function). Pure functions; the GPU tests check them
// against their C++ mirrors.

#include "render/gpu/bloom_shared.h"
#include "render/gpu/eye_adaptation_shared.h"

// Must match GpuEyeAdaptation: the exposure, its target and the scene's mean in stops, 1 once measured, and the dark
// adaptation's wait.
struct EyeAdaptation {
    float exposureEv;
    float targetEv;
    float meanEv;
    float measured;
    float darkHeldSeconds;
    float unused0;
    float unused1;
    float unused2;
};

float pixelLuminance(vec3 colour) {
    return BLOOM_LUMA_RED * colour.r + BLOOM_LUMA_GREEN * colour.g + BLOOM_LUMA_BLUE * colour.b;
}

int histogramBin(float luminance) {
    if (!(luminance > 0.0)) {
        return 0;
    }

    float share = (log2(luminance) - EYE_HISTOGRAM_LOWEST_EV) / (EYE_HISTOGRAM_HIGHEST_EV - EYE_HISTOGRAM_LOWEST_EV);
    return min(int(floor(clamp(share, 0.0, 1.0) * float(EYE_HISTOGRAM_BINS))), EYE_HISTOGRAM_BINS - 1);
}

float histogramBinEv(int bin) {
    return EYE_HISTOGRAM_LOWEST_EV + (float(bin) + 0.5) / float(EYE_HISTOGRAM_BINS) * (EYE_HISTOGRAM_HIGHEST_EV - EYE_HISTOGRAM_LOWEST_EV);
}

float histogramMeanEv(uint counts[EYE_HISTOGRAM_BINS], float lowShare, float highShare, float fallbackEv) {
    float total = 0.0;

    for (int bin = 0; bin < EYE_HISTOGRAM_BINS; ++bin) {
        total += float(counts[bin]);
    }

    float spanStart = lowShare * total;
    float spanEnd = highShare * total;
    float before = 0.0;
    float weighted = 0.0;
    float weight = 0.0;

    for (int bin = 0; bin < EYE_HISTOGRAM_BINS; ++bin) {
        float after = before + float(counts[bin]);
        float inside = max(min(after, spanEnd) - max(before, spanStart), 0.0);
        weighted += inside * histogramBinEv(bin);
        weight += inside;
        before = after;
    }

    return weight > 0.0 ? weighted / weight : fallbackEv;
}

float targetExposureEv(float meanEv, float referenceEv, float strength, float lowestEv, float highestEv) {
    return clamp(strength * (referenceEv - meanEv), lowestEv, highestEv);
}

float adaptExposureEv(float currentEv, float targetEv, float seconds, float darkSeconds, float brightSeconds) {
    float timeConstant = targetEv > currentEv ? darkSeconds : brightSeconds;

    if (timeConstant <= 0.0) {
        return targetEv;
    }

    return currentEv + (targetEv - currentEv) * (1.0 - exp(-seconds / timeConstant));
}

// The exposure after the step (x) and the dark adaptation's wait (y).
vec2 holdThenAdaptExposure(float currentEv, float targetEv, float heldSeconds, float seconds, float darkSeconds, float brightSeconds,
                           float delaySeconds) {
    if (targetEv <= currentEv) {
        return vec2(adaptExposureEv(currentEv, targetEv, seconds, darkSeconds, brightSeconds), 0.0);
    }

    float held = heldSeconds + seconds;
    float adapting = max(held - max(heldSeconds, delaySeconds), 0.0);
    return vec2(adapting > 0.0 ? adaptExposureEv(currentEv, targetEv, adapting, darkSeconds, brightSeconds) : currentEv, held);
}

#endif
