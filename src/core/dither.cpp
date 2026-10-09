#include "core/dither.h"
#include "core/srgb.h"

#include <algorithm>
#include <cmath>

namespace {
    float fraction(float value) {
        return value - std::floor(value);
    }
}

/*
* Jorge Jimenez's interleaved gradient noise (from Call of Duty: Advanced Warfare): two nested "keep the fraction" steps
* with carefully chosen constants. Neighbouring pixels get very different values, and every small patch covers the
* range evenly, so the grain is fine and even instead of clumpy like plain random noise.
*/
float interleavedGradientNoise(float pixelX, float pixelY) {
    return fraction(52.9829189f * fraction(0.06711056f * pixelX + 0.00583715f * pixelY));
}

/*
* The screen stores sRGB values in 256 steps, so the noise is sized in those steps: encode, add up to half a step either
* way, decode. Kept just under a full step, so a value exactly on a level can never be pushed onto the rounding boundary.
*/
float ditherForEightBits(float linear, float noise) {
    const float encoded = linearToSrgb(std::max(linear, 0.0f));
    const float nudged = encoded + (noise - 0.5f) * (0.99f / 255.0f);

    return srgbToLinear(std::clamp(nudged, 0.0f, 1.0f));
}