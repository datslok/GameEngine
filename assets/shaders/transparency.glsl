// Transparent surfaces' blending, included with #include "transparency.glsl": mirrors render/gpu/transparency.cpp,
// which tests it, and tests/gpu checks these functions against it on the GPU.

#ifndef TRANSPARENCY_GLSL
#define TRANSPARENCY_GLSL

#include "render/gpu/transparency_shared.h"

// The light a transparent fragment adds, and the share of what is behind that still shows (per channel).
struct TransparentLight {
    vec3 added;
    vec3 transmittance;
};

// Coverage scales the whole surface; transmission swaps its diffuse part for what is behind, tinted, less what it
// reflects. Metals pass no light.
TransparentLight transparentBlend(float alpha, float transmission, float metallic, vec3 diffuseLight, vec3 specularLight, vec3 emissive,
                                  vec3 baseColour, vec3 specularAlbedo) {
    float passing = transmission * (1.0 - metallic);
    vec3 surface = diffuseLight * (1.0 - passing) + specularLight + emissive;
    vec3 through = baseColour * (vec3(1.0) - specularAlbedo) * passing;
    return TransparentLight(surface * alpha, through * alpha + vec3(1.0) * (1.0 - alpha));
}

// The reactive mask's share of one pixel: the change as a share of the brighter of before and after, the most changed
// channel, scaled, at most the limit.
float reactiveShare(vec3 opaque, vec3 blended, float scale, float limit) {
    vec3 before = max(opaque, vec3(0.0));
    vec3 after = max(blended, vec3(0.0));
    vec3 changed = abs(after - before) / max(max(before, after), vec3(float(TRANSPARENCY_REACTIVE_FLOOR)));
    return clamp(max(changed.r, max(changed.g, changed.b)) * scale, 0.0, limit);
}

#endif
