// Physically based shading, included with #include "brdf.glsl": the arithmetic mirrors render/gpu/brdf.cpp, which tests
// the physics (energy kept, reciprocity, Fresnel), and tests/gpu checks these functions against it on the GPU.
// Pure functions only (no textures, no pixel position), so a compute shader can run them on fixed inputs.

#ifndef BRDF_GLSL
#define BRDF_GLSL

#include "render/gpu/brdf_shared.h"

const float pi = 3.14159265358979;

/*
* GGX: the density of microfacet normals along the halfway vector, normalised so the facets' projected area is the
* surface's own. alpha is roughness squared.
*/
float ggxDistribution(float nDotH, float alpha) {
    float alphaSquared = alpha * alpha;
    float denominator = nDotH * nDotH * (alphaSquared - 1.0) + 1.0;
    return alphaSquared / (pi * denominator * denominator);
}

// Smith's height-correlated shadowing-masking (Heitz 2014), already divided by 4 (n.l)(n.v).
float smithVisibility(float nDotV, float nDotL, float alpha) {
    float alphaSquared = alpha * alpha;
    float towardsView = nDotL * sqrt(nDotV * nDotV * (1.0 - alphaSquared) + alphaSquared);
    float towardsLight = nDotV * sqrt(nDotL * nDotL * (1.0 - alphaSquared) + alphaSquared);
    return 0.5 / (towardsView + towardsLight);
}

// Schlick's Fresnel: f0 head-on, all light at grazing.
vec3 schlickFresnel(vec3 f0, float cosine) {
    float grazing = pow(1.0 - clamp(cosine, 0.0, 1.0), 5.0);
    return f0 + (1.0 - f0) * grazing;
}

// The cosine between the normal and the way to the eye, kept just above zero so an eye along the surface stays finite.
float viewCosine(vec3 normal, vec3 toCamera) {
    return clamp(dot(normal, toCamera), BRDF_SMALLEST_VIEW_COSINE, 1.0);
}

/*
* What every light shares at this pixel (prepareShading in brdf.cpp): the reflectance head-on (f0), the GGX width, the
* compensation for light the single-scattering model loses between facets (Turquin 2019), the specular part's albedo,
* and the diffuse colour weighted by what the specular part leaves.
*/
struct ShadingPoint {
    vec3 normal;
    vec3 toCamera;
    float nDotV;
    vec3 f0;
    float alpha;
    vec3 energyCompensation;
    vec3 specularAlbedo;
    vec3 diffuse;
};

// tableEntry is the specular albedo table's scale and bias at viewCosine(normal, toCamera) and this roughness.
ShadingPoint prepareShading(vec3 baseColour, float metallic, float roughness, vec3 normal, vec3 toCamera, vec2 tableEntry) {
    ShadingPoint point;
    point.normal = normal;
    point.toCamera = toCamera;
    point.nDotV = viewCosine(normal, toCamera);
    point.alpha = roughness * roughness;
    point.f0 = mix(vec3(BRDF_DIELECTRIC_REFLECTANCE), baseColour, metallic);

    point.energyCompensation = 1.0 + point.f0 * (1.0 / (tableEntry.x + tableEntry.y) - 1.0);
    point.specularAlbedo = (point.f0 * tableEntry.x + tableEntry.y) * point.energyCompensation;
    point.diffuse = baseColour * (1.0 - metallic) * (1.0 - point.specularAlbedo);
    return point;
}

// Whether a light along toLight reaches the side of the surface the eye sees (facesLight in brdf.cpp): grazing or from
// behind gives nothing, so callers skip such a light before its shadow lookups.
bool facesLight(ShadingPoint point, vec3 toLight) {
    return dot(point.normal, toLight) > 0.0;
}

/*
* One light's contribution, its radiance already scaled by distance falloff (and the cone, for spotlights). The diffuse
* part scatters evenly, so it does not depend on the viewer; the specular part is D V F, brightest where the halfway
* vector between the light and the viewer lines up with the surface normal. Both are pi times the textbook BRDF (light
* values are the irradiance they give a surface facing them), so a white matte surface lit head-on by 1 shows 1.
*/
void addLight(vec3 radiance, vec3 toLight, ShadingPoint point, inout vec3 diffuse, inout vec3 specular) {
    // No light, and so no highlight, on the side facing away.
    if (!facesLight(point, toLight)) {
        return;
    }

    float nDotL = dot(point.normal, toLight);

    vec3 incoming = radiance * nDotL;
    diffuse += point.diffuse * incoming;

    vec3 sum = toLight + point.toCamera;
    float sumLength = length(sum);

    if (sumLength > 0.0) {
        vec3 halfway = sum / sumLength;
        float nDotH = max(dot(point.normal, halfway), 0.0);
        float vDotH = max(dot(point.toCamera, halfway), 0.0);
        vec3 brdf = schlickFresnel(point.f0, vDotH) * (ggxDistribution(nDotH, point.alpha) * smithVisibility(point.nDotV, nDotL, point.alpha));
        specular += brdf * point.energyCompensation * incoming * pi;
    }
}

// Ambient light comes from every direction alike, so the surface returns its albedo of it: the weighted diffuse colour
// plus the specular albedo (a metal, with no diffuse part, still reflects it).
vec3 shadeAmbient(ShadingPoint point, vec3 ambient) {
    return ambient * (point.diffuse + point.specularAlbedo);
}

vec3 shadeProbeAmbient(ShadingPoint point, vec3 diffuseLight, vec3 specularLight) {
    return diffuseLight * point.diffuse + specularLight * point.specularAlbedo;
}

#endif
