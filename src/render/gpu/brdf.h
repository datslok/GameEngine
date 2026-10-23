#pragma once

#include "math/vec2.h"
#include "math/vec3.h"
#include "render/gpu/brdf_shared.h"

#include <cstdint>
#include <vector>

/*
* Physically based shading: how much of the light arriving at a surface from one direction leaves towards the eye.
* That ratio is the BRDF (bidirectional reflectance distribution function), f(l, v). These functions mirror triangle.frag
* on the CPU, so tests can check the physics (energy is never made, reciprocity, Fresnel's limits) without a GPU.
*
* The model is glTF's metallic-roughness one. Up close every surface is a landscape of tiny mirrors (microfacets) facing
* in random directions; light reaches the eye only off facets whose normal is the half vector h between the light and
* the eye. The specular part is
*     f_spec = D(h) * G(l, v) * F(v.h) / (4 (n.l)(n.v))
* with D the share of facets facing h (GGX), G the share of those that neither the light nor the eye sees blocked by
* other facets (Smith, height-correlated) and F how much a mirror reflects rather than lets in (Fresnel, Schlick's fit).
* Light that gets in scatters under the surface and leaves in every direction: the diffuse part, Lambert's
* f_diff = colour / pi. Metals absorb whatever gets in, so they have no diffuse part and colour their reflection instead.
*
* Units: a light's radiance is the irradiance it gives a surface facing it, and the shader returns pi times the
* textbook result, so a white Lambert surface lit head-on by a light of 1 shows 1. Light and exposure values tuned for
* the earlier shading keep their look.
*/

constexpr float dielectricReflectance = static_cast<float>(BRDF_DIELECTRIC_REFLECTANCE);
constexpr float smallestRoughness = static_cast<float>(BRDF_SMALLEST_ROUGHNESS);

// The precomputed table's size (texels per side) and how many directions each texel averages when it is built.
constexpr std::uint32_t brdfTableSize = BRDF_TABLE_SIZE;
constexpr std::uint32_t brdfTableSamples = 4096;

// A surface after its textures are applied: linear base colour, metallic and roughness, each 0..1.
struct SurfaceProperties {
    Vec3 baseColour{1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
};

// Roughness limited to smallestRoughness..1.
float clampRoughness(float roughness);

// GGX's width alpha: roughness squared, which artists find spreads evenly (half roughness looks half as rough).
float ggxAlpha(float roughness);

/*
* GGX (Trowbridge-Reitz) normal distribution: the density of facet normals at angle acos(nDotH) from the surface
* normal, per unit solid angle, normalised so the facets' area projected onto the surface equals the surface's area:
* the integral of D(h) (n.h) over the hemisphere is 1.
*/
float ggxDistribution(float nDotH, float alpha);

/*
* Smith's shadowing-masking for GGX, height-correlated (Heitz 2014), already divided by 4 (n.l)(n.v): the "visibility"
* V = G / (4 (n.l)(n.v)), so the specular BRDF is D V F. Symmetric in its two cosines.
*/
float smithVisibility(float nDotV, float nDotL, float alpha);

// Schlick's fit to the Fresnel equations: f0 head-on (cosine 1), rising to 1 at grazing (cosine 0).
Vec3 schlickFresnel(const Vec3& f0, float cosine);

// The reflectance head-on: dielectricReflectance for non-metals, the base colour for metals, blended by metallic.
Vec3 reflectanceAtNormal(const SurfaceProperties& surface);

// The colour of the light a surface scatters from under it (its diffuse colour): metals have none. What rays' hits are
// lit with (matte only), and the diffuse part's colour before the specular part takes its share.
Vec3 matteColour(const Vec3& baseColour, float metallic);

/*
* The van der Corput radical inverse: the bits of a number mirrored behind the binary point (1 to 0.5, 2 to 0.25, 3 to
* 0.75). With (k + 0.5) / count it makes the Hammersley point set, which spreads points over the unit square far more
* evenly than random numbers do, so an average over them converges about as 1 / count instead of 1 / sqrt(count).
*/
float radicalInverse(std::uint32_t bits);

/*
* The sample-th of samples GGX facet normals round a normal along +y, drawn with probability D(h) (n.h) along the
* Hammersley set: the GGX inversion cos^2 theta = (1 - u) / (1 + (alpha^2 - 1) u), turned by 2 pi times the radical
* inverse. The BRDF table and the reflection probe's prefilter both average over them.
*/
Vec3 ggxSampleHalfway(std::uint32_t sample, std::uint32_t samples, float alpha);

// A direction given round +y turned to lie round a unit normal instead, through any frame with the normal as its y.
Vec3 alongNormal(const Vec3& local, const Vec3& normal);

// The single-scattering specular BRDF D V F for unit vectors, zero where either the light or the eye is below the surface.
Vec3 specularBrdf(const Vec3& f0, float alpha, const Vec3& normal, const Vec3& toLight, const Vec3& toCamera);

/*
* The specular part's directional albedo (the share of light from everywhere it sends towards the eye) as
* f0 * scale + bias, tabulated over n.v (columns) and roughness (rows), texel i at i / (size - 1), so the first and
* last texels hold 0 and 1 exactly (n.v = 0 is taken just above zero, as the shader clamps it).
* scale + bias is the albedo of a perfect mirror's facets (f0 = 1); below 1 for rough surfaces, because light reflected
* into another facet is dropped by the single-scattering model (energyCompensation puts it back).
*/
struct BrdfTable {
    std::uint32_t size = 0;
    std::vector<Vec2> entries; // [row * size + column]: x scale, y bias
};

/*
* Integrates the table numerically: for each texel, samples (brdfTableSamples by default, which keeps it within 0.003 of the exact integral even at grazing views) facet normals in proportion to D (n.h) along a Hammersley
* sequence and averages each reflected direction's weight. Throws std::invalid_argument for a size below 2 or no samples.
*/
BrdfTable buildBrdfTable(std::uint32_t size = brdfTableSize, std::uint32_t samples = brdfTableSamples);

// The table read as the shader reads it: bilinear between the grid points, clamped at 0 and 1.
Vec2 sampleBrdfTable(const BrdfTable& table, float nDotV, float roughness);

/*
* What the shader works out once per pixel, before its loop over lights. The single-scattering model loses the light
* that bounces between facets more than once (Turquin 2019): scaling the specular part by 1 + f0 (1 / E - 1), where E is
* the facets' own albedo (scale + bias), puts that light back, so a white metal reflects all of it at every roughness.
* The diffuse part gets what the specular part leaves: diffuse colour times (1 - specular albedo), so a white non-metal
* returns exactly all the light it receives.
*/
struct ShadingPoint {
    Vec3 normal;
    Vec3 toCamera;
    float nDotV = 0.0f; // kept just above zero, so an eye along the surface stays finite
    Vec3 f0;
    float alpha = 0.0f;
    Vec3 energyCompensation;
    Vec3 specularAlbedo; // with multiple scattering
    Vec3 diffuse;        // diffuse colour already weighted by what the specular part leaves
};

ShadingPoint prepareShading(const SurfaceProperties& surface, const Vec3& normal, const Vec3& toCamera, const BrdfTable& table);

struct LightResponse {
    Vec3 diffuse;
    Vec3 specular;
};

// Whether a light along toLight reaches the side of the surface the eye sees: grazing (a cosine of exactly 0) or from behind
// gives nothing. Asked before a light's shadow lookups, which a light giving nothing would waste.
bool facesLight(const ShadingPoint& point, const Vec3& toLight);

// One light's reflected light towards the eye, for a light of the given radiance (irradiance when facing it) along toLight.
LightResponse shadeLight(const ShadingPoint& point, const Vec3& toLight, const Vec3& radiance);

// The reflected light under uniform light of the given radiance from every direction.
Vec3 shadeAmbient(const ShadingPoint& point, const Vec3& ambient);

/*
* The same lit by the light probes instead (render/gpu/ddgi_sampling.h, ProbeLight): their irradiance for the diffuse
* part, and the specular part's stand-in (the probes read in the mirror direction) until reflections exist. Kept apart
* from shadeAmbient, whose grouping pictures without probes keep, so they stay the same to the last bit.
*/
Vec3 shadeProbeAmbient(const ShadingPoint& point, const Vec3& diffuseLight, const Vec3& specularLight);

// The table as the GPU stores it: two 16-bit numbers per texel (scale, bias) for 0..1, row by row (rows are roughness).
std::vector<std::uint16_t> brdfTableTexels(const BrdfTable& table);
