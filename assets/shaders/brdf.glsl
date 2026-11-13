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

// The colour a surface scatters from under it (matteColour in brdf.cpp): metals have none.
vec3 matteColour(vec3 baseColour, float metallic) {
    return baseColour * (1.0 - metallic);
}

// Schlick's Fresnel: f0 head-on, all light at grazing.
vec3 schlickFresnel(vec3 f0, float cosine) {
    float grazing = pow(1.0 - clamp(cosine, 0.0, 1.0), 5.0);
    return f0 + (1.0 - f0) * grazing;
}

// The van der Corput radical inverse: the bits mirrored behind the binary point (Hammersley points' second coordinate).
float radicalInverse(uint bits) {
    return float(bitfieldReverse(bits)) * 2.3283064365386963e-10;
}

// The index-th of samples GGX facet normals round a normal along +y (the GGX inversion along the Hammersley set).
vec3 ggxSampleHalfway(uint index, uint samples, float alpha) {
    float u = (float(index) + 0.5) / float(samples);
    float angle = 2.0 * pi * radicalInverse(index);
    float cosineSquared = (1.0 - u) / (1.0 + (alpha * alpha - 1.0) * u);
    float cosine = sqrt(cosineSquared);
    float sine = sqrt(max(1.0 - cosineSquared, 0.0));
    return vec3(sine * cos(angle), cosine, sine * sin(angle));
}

// A direction given round +y turned to lie round a unit normal, through a frame with a helper axis never along it.
vec3 alongNormal(vec3 local, vec3 normal) {
    vec3 helper = abs(normal.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(helper, normal));
    vec3 bitangent = cross(normal, tangent);
    return tangent * local.x + normal * local.y + bitangent * local.z;
}

// The cosine between the normal and the way to the eye, kept just above zero so an eye along the surface stays finite.
float viewCosine(vec3 normal, vec3 toCamera) {
    return clamp(dot(normal, toCamera), BRDF_SMALLEST_VIEW_COSINE, 1.0);
}

/*
* What every light shares at this pixel (prepareShading in brdf.cpp): the reflectance head-on (f0), the GGX width, the
* compensation for light the single-scattering model loses between facets (Turquin 2019), the specular part's albedo,
* and the diffuse colour weighted by what the specular part leaves; and a clear coat over it, none until addClearCoat
* lays one on (its amount, normal, view cosine, width, compensation and albedo).
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
    float coat;
    vec3 coatNormal;
    float coatNDotV;
    float coatAlpha;
    float coatCompensation;
    float coatAlbedo;
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
    point.diffuse = matteColour(baseColour, metallic) * (1.0 - point.specularAlbedo);

    point.coat = 0.0;
    point.coatNormal = normal;
    point.coatNDotV = point.nDotV;
    point.coatAlpha = point.alpha;
    point.coatCompensation = 1.0;
    point.coatAlbedo = 0.0;
    return point;
}

/*
* A clear coat laid over the point (addClearCoat in brdf.cpp): its share of the light from everywhere is its amount times
* the table's albedo for a 4% reflector (coatEntry, read at viewCosine(normal, toCamera) and the coat's roughness, already
* clamped), and the base keeps the rest. A coat of nothing leaves the point as it was.
*/
void addClearCoat(inout ShadingPoint point, float amount, float roughness, vec3 normal, vec2 coatEntry) {
    if (!(amount > 0.0)) {
        return;
    }

    point.coat = min(amount, 1.0);
    point.coatNormal = normal;
    point.coatNDotV = viewCosine(normal, point.toCamera);
    point.coatAlpha = roughness * roughness;
    point.coatCompensation = 1.0 + BRDF_DIELECTRIC_REFLECTANCE * (1.0 / (coatEntry.x + coatEntry.y) - 1.0);
    point.coatAlbedo = point.coat * (BRDF_DIELECTRIC_REFLECTANCE * coatEntry.x + coatEntry.y) * point.coatCompensation;

    float baseShare = 1.0 - point.coatAlbedo;
    point.energyCompensation *= baseShare;
    point.specularAlbedo *= baseShare;
    point.diffuse *= baseShare;
}

// Whether the reflection pass follows the coat's reflection rather than the base's: the smoother, the coat on a tie.
bool reflectsCoat(ShadingPoint point) {
    return point.coat > 0.0 && point.coatAlpha <= point.alpha;
}

// Whether a light along toLight reaches the side of the surface the eye sees (facesLight in brdf.cpp), either layer's
// with a clear coat: grazing or from behind gives nothing, so callers skip such a light before its shadow lookups.
bool facesLight(ShadingPoint point, vec3 toLight) {
    return dot(point.normal, toLight) > 0.0 || (point.coat > 0.0 && dot(point.coatNormal, toLight) > 0.0);
}

/*
* One light's contribution, its radiance already scaled by distance falloff (and the cone, for spotlights). The diffuse
* part scatters evenly, so it does not depend on the viewer; the specular part is D V F, brightest where the halfway
* vector between the light and the viewer lines up with the surface normal. Both are pi times the textbook BRDF (light
* values are the irradiance they give a surface facing them), so a white matte surface lit head-on by 1 shows 1. A clear
* coat adds its own white highlight about its own normal; each layer is lit only from in front of itself.
*/
void addLight(vec3 radiance, vec3 toLight, ShadingPoint point, inout vec3 diffuse, inout vec3 specular) {
    float nDotL = dot(point.normal, toLight);
    vec3 sum = toLight + point.toCamera;
    float sumLength = length(sum);

    if (nDotL > 0.0) {
        vec3 incoming = radiance * nDotL;
        diffuse += point.diffuse * incoming;

        if (sumLength > 0.0) {
            vec3 halfway = sum / sumLength;
            float nDotH = max(dot(point.normal, halfway), 0.0);
            float vDotH = max(dot(point.toCamera, halfway), 0.0);
            vec3 brdf = schlickFresnel(point.f0, vDotH) * (ggxDistribution(nDotH, point.alpha) * smithVisibility(point.nDotV, nDotL, point.alpha));
            specular += brdf * point.energyCompensation * incoming * pi;
        }
    }

    if (point.coat > 0.0) {
        float coatNDotL = dot(point.coatNormal, toLight);

        if (coatNDotL > 0.0 && sumLength > 0.0) {
            vec3 halfway = sum / sumLength;
            float nDotH = max(dot(point.coatNormal, halfway), 0.0);
            float vDotH = max(dot(point.toCamera, halfway), 0.0);
            vec3 brdf = schlickFresnel(vec3(BRDF_DIELECTRIC_REFLECTANCE), vDotH) *
                        (ggxDistribution(nDotH, point.coatAlpha) * smithVisibility(point.coatNDotV, coatNDotL, point.coatAlpha));
            specular += brdf * (point.coat * point.coatCompensation) * radiance * (pi * coatNDotL);
        }
    }
}

// Ambient light comes from every direction alike, so the surface returns its albedo of it: the weighted diffuse colour
// plus the specular albedo (a metal, with no diffuse part, still reflects it), plus a clear coat's.
vec3 shadeAmbient(ShadingPoint point, vec3 ambient) {
    vec3 reflected = ambient * (point.diffuse + point.specularAlbedo);

    if (point.coat > 0.0) {
        reflected += ambient * point.coatAlbedo;
    }

    return reflected;
}

// A clear coat reflects the mirror light too, the base's stand-in.
vec3 shadeProbeAmbient(ShadingPoint point, vec3 diffuseLight, vec3 specularLight) {
    vec3 reflected = diffuseLight * point.diffuse + specularLight * point.specularAlbedo;

    if (point.coat > 0.0) {
        reflected += specularLight * point.coatAlbedo;
    }

    return reflected;
}

#endif
