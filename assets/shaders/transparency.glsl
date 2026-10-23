// Transparent surfaces' blending, included with #include "transparency.glsl": mirrors render/gpu/transparency.cpp,
// which tests it, and tests/gpu checks these functions against it on the GPU.

#ifndef TRANSPARENCY_GLSL
#define TRANSPARENCY_GLSL

#include "render/gpu/transparency_shared.h"

// schlickFresnel, for the light crossing glass.
#include "brdf.glsl"

// The light a transparent fragment adds, and the share of what is behind that still shows (per channel).
struct TransparentLight {
    vec3 added;
    vec3 transmittance;
};

// The share of what is behind that still shows: shared by the blend and the light crossing glass.
vec3 transparentTransmittance(float alpha, float transmission, float metallic, vec3 baseColour, vec3 specularAlbedo) {
    float passing = transmission * (1.0 - metallic);
    vec3 through = baseColour * (vec3(1.0) - specularAlbedo) * passing;
    return through * alpha + vec3(1.0) * (1.0 - alpha);
}

// Coverage scales the whole surface; transmission swaps its diffuse part for what is behind, tinted, less what it
// reflects. Metals pass no light.
TransparentLight transparentBlend(float alpha, float transmission, float metallic, vec3 diffuseLight, vec3 specularLight, vec3 emissive,
                                  vec3 baseColour, vec3 specularAlbedo) {
    float passing = transmission * (1.0 - metallic);
    vec3 surface = diffuseLight * (1.0 - passing) + specularLight + emissive;
    return TransparentLight(surface * alpha, transparentTransmittance(alpha, transmission, metallic, baseColour, specularAlbedo));
}

// Light crossing one pane at this cosine to its normal (either side): the reflected share is Schlick's Fresnel there.
vec3 glassLightTransmittance(float alpha, float transmission, float metallic, vec3 baseColour, float cosine) {
    vec3 headOn = mix(vec3(BRDF_DIELECTRIC_REFLECTANCE), baseColour, metallic);
    return transparentTransmittance(alpha, transmission, metallic, baseColour, schlickFresnel(headOn, abs(cosine)));
}

// A face turned towards the light counts (the ray towards the light meets it from behind); a double-sided one always.
bool glassFaceCounts(bool rayMeetsFront, bool doubleSided) {
    return !rayMeetsFront || doubleSided;
}

bool segmentMeetsSphere(vec3 from, vec3 direction, float segmentLength, vec3 centre, float radius) {
    float along = clamp(dot(centre - from, direction), 0.0, segmentLength);
    vec3 gap = from + direction * along - centre;
    return dot(gap, gap) <= radius * radius;
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
