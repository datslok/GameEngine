// Transparent surfaces' blending, included with #include "transparency.glsl": mirrors render/gpu/transparency.cpp,
// which tests it, and tests/gpu checks these functions against it on the GPU.

#ifndef TRANSPARENCY_GLSL
#define TRANSPARENCY_GLSL

#include "render/gpu/transparency_shared.h"
#include "render/gpu/depth_of_field_shared.h"

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

// Beer's law: what light keeps after length through a medium keeping colour after 1 / inverseDistance.
vec3 beerAttenuation(vec3 colour, float inverseDistance, float pathLength) {
    return pow(colour, vec3(pathLength * inverseDistance));
}

// The tangent of rough glass's spread: (ior - 1) times GGX's alpha (a thin prism's deviation per facet tilt).
float transmissionSpread(float roughness, float ior) {
    return max(ior - 1.0, 0.0) * roughness * roughness;
}

// The radius in pixels of the disc a point behindDistance behind frosted glass spreads over on it, and its ceiling.
float frostedRadius(float spread, float behindDistance, float viewDistance, float pixelsPerRadian) {
    return spread * behindDistance / max(viewDistance + behindDistance, 1e-6) * pixelsPerRadian;
}

float frostedReach(float spread, float pixelsPerRadian) {
    return spread * pixelsPerRadian;
}

// The blurred picture's level: log2 of the disc's width in pixels.
float refractionLevel(float spread, float behindDistance, float viewDistance, float pixelsPerRadian, uint levels) {
    float pixels = 2.0 * frostedRadius(spread, behindDistance, viewDistance, pixelsPerRadian);
    return clamp(log2(max(pixels, 1.0)), 0.0, float(levels) - 1.0);
}

// The level frosted glass's gather reads its samples from: the rings' spacing about a texel there.
float frostedGatherLevel(float reach, uint levels) {
    float spacing = reach / float(DEPTH_OF_FIELD_RINGS);
    return clamp(log2(max(spacing, 1.0)), 0.0, float(levels) - 1.0);
}

// A smooth read along one axis (bsplineAxis in transparency.cpp): xy the two bilinear reads' coordinates, zw their weights.
vec4 bsplineAxis(float coordinate, float size) {
    float x = coordinate * size - 0.5;
    float whole = floor(x);
    float f = x - whole;
    float w0 = (1.0 - f) * (1.0 - f) * (1.0 - f) / 6.0;
    float w1 = (3.0 * f * f * f - 6.0 * f * f + 4.0) / 6.0;
    float w2 = (-3.0 * f * f * f + 3.0 * f * f + 3.0 * f + 1.0) / 6.0;
    float w3 = f * f * f / 6.0;
    float firstWeight = w0 + w1;
    float secondWeight = w2 + w3;
    float firstTexel = whole - 1.0 + w1 / firstWeight;
    float secondTexel = whole + 1.0 + w3 / secondWeight;
    return vec4((firstTexel + 0.5) / size, (secondTexel + 0.5) / size, firstWeight, secondWeight);
}

// Where the view ray leaves thick glass (Filament's solid sphere): bent entering, then the thickness times the cosine
// to the inward normal.
vec3 solidRefractionExit(vec3 position, vec3 toCamera, vec3 normal, float ior, float thickness) {
    vec3 inside = refract(-toCamera, normal, 1.0 / ior);
    return position + inside * (thickness * -dot(normal, inside));
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
