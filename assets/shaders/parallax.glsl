// Parallax occlusion mapping, included with #include "parallax.glsl": the arithmetic mirrors render/gpu/parallax.cpp,
// which tests it, and tests/gpu checks these functions against it on the GPU. Pure functions only: the searches' loops,
// which read the height map, are in the main pass (scene_surface.glsl), mirrored by parallaxSearch and parallaxShadow.

#ifndef PARALLAX_GLSL
#define PARALLAX_GLSL

#include "render/gpu/parallax_shared.h"

// A direction in the surface's frame (x tangent, y bitangent, z normal), the frame made orthonormal first.
vec3 toTangentFrame(vec3 direction, vec3 normal, vec4 tangent) {
    vec3 unitNormal = normalize(normal);
    vec3 unitTangent = normalize(tangent.xyz - unitNormal * dot(unitNormal, tangent.xyz));
    vec3 bitangent = cross(unitNormal, unitTangent) * tangent.w;
    return vec3(dot(direction, unitTangent), dot(direction, bitangent), dot(direction, unitNormal));
}

// Steps for a view at this cosine to the normal: the fewest straight down, mostSteps along the surface.
int parallaxSteps(float viewCosine, int mostSteps) {
    int most = max(mostSteps, PARALLAX_FEWEST_STEPS);
    float steps = float(most) + (float(PARALLAX_FEWEST_STEPS) - float(most)) * clamp(viewCosine, 0.0, 1.0);
    return clamp(int(floor(steps + 0.5)), PARALLAX_FEWEST_STEPS, most);
}

// The shift in texture units over the whole depth, away from the eye.
vec2 parallaxDelta(vec3 viewTangent, float depth) {
    float scale = -depth / max(viewTangent.z, float(PARALLAX_SMALLEST_VIEW_COSINE));
    return viewTangent.xy * scale;
}

// The shift per unit of height for a ray rising towards a light.
vec2 parallaxLightDelta(vec3 lightTangent, float depth) {
    return parallaxDelta(lightTangent, -depth);
}

// How far the ray at t is above the ground of this height.
float parallaxGap(float t, float height) {
    return 1.0 - t - height;
}

// Where between the last step above the ground and the first below the ray crosses it, as a share of the step.
float parallaxCrossing(float gapAbove, float gapBelow) {
    float span = gapAbove - gapBelow;
    return span > 0.0 ? clamp(gapAbove / span, 0.0, 1.0) : 1.0;
}

// The depth seen head-on in pixels.
float parallaxDepthPixels(float depth, vec2 uvStepAcross, vec2 uvStepDown) {
    float footprint = max(length(uvStepAcross), length(uvStepDown));
    return depth / max(footprint, 1e-12);
}

// How much of the depth to use at that many pixels.
float parallaxFade(float depthPixels) {
    return smoothstep(float(PARALLAX_FADE_NONE_PIXELS), float(PARALLAX_FADE_FULL_PIXELS), depthPixels);
}

// One self-shadowing sample: the light's share past a bump of this height.
float parallaxShadowSample(float terrainHeight, float rayHeight, float climbed) {
    if (terrainHeight <= rayHeight) {
        return 1.0;
    }

    return clamp(1.0 - (terrainHeight - rayHeight) / climbed * float(PARALLAX_SHADOW_SOFTNESS), 0.0, 1.0);
}

#endif
