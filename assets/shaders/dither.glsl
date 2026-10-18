// Shader functions shared by several shaders, included with #include "dither.glsl": per-pixel noise, the sRGB curve, and
// the dither that uses both. Mirrors core/srgb.cpp and core/dither.cpp, which test the maths.

#ifndef DITHER_GLSL
#define DITHER_GLSL

#include "core/dither_shared.h"

// The sRGB curve, as in core/srgb.cpp.
vec3 linearToSrgb(vec3 linear) {
    return mix(linear * 12.92, 1.055 * pow(linear, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), linear));
}

vec3 srgbToLinear(vec3 encoded) {
    return mix(encoded / 12.92, pow((encoded + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), encoded));
}

// Noise from a pixel's position, 0..1: neighbouring pixels get very different values, and every small patch covers the
// range evenly. Used for the dither and to turn each pixel's soft shadow samples.
float interleavedGradientNoise(vec2 pixel) {
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

/*
* Dithering against banding. The screen keeps 256 levels per channel, and a slow, dark fade (the flashlight's pool at night)
* changes by less than one level over many pixels, so it rounds into visible rings. Adding a little noise first, up to half
* a level either way and different per pixel, makes nearby pixels round to the two levels around the true value in the
* right proportion, and the eye averages them: the rings become fine grain.
*/
vec3 ditherForEightBits(vec3 linear) {
    vec3 encoded = linearToSrgb(max(linear, vec3(0.0)));
    float noise = interleavedGradientNoise(gl_FragCoord.xy);
    vec3 nudged = encoded + (noise - 0.5) * (DITHER_STEP_FRACTION / DITHER_LEVELS);
    return srgbToLinear(clamp(nudged, 0.0, 1.0));
}

#endif
