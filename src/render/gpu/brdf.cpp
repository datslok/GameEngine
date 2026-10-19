#include "render/gpu/brdf.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {
    constexpr float pi = std::numbers::pi_v<float>;

    constexpr float smallestViewCosine = static_cast<float>(BRDF_SMALLEST_VIEW_COSINE);

    /*
    * The same sum written out from the cosines: D V F, as the shader computes it once it has them. nDotL and nDotV
    * must be positive.
    */
    Vec3 specularFromCosines(const Vec3& f0, float alpha, float nDotV, float nDotL, float nDotH, float vDotH) {
        return schlickFresnel(f0, vDotH) * (ggxDistribution(nDotH, alpha) * smithVisibility(nDotV, nDotL, alpha));
    }

    /*
    * The Hammersley point set: the k-th point is (k + 0.5) / count against the bits of k mirrored behind the binary
    * point (the van der Corput sequence). It spreads points over the unit square far more evenly than random numbers
    * do, so an average over them converges about as 1 / count instead of 1 / sqrt(count).
    */
    float radicalInverse(std::uint32_t bits) {
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        return static_cast<float>(bits) * 2.3283064365386963e-10f;
    }

    /*
    * One texel: the specular albedo's scale and bias at one view cosine and roughness, with the normal along +y.
    * Facet normals h are drawn with probability D(h) (n.h) (the GGX inversion: cos^2 theta = (1 - u) / (1 + (alpha^2 - 1) u)),
    * so each reflected light direction l arrives with density D (n.h) / (4 v.h) and its share of D V F (n.l) is
    * 4 V (n.l)(v.h) / (n.h): D cancels, which keeps the sum steady even for sharp highlights. Fresnel's (1 - v.h)^5 splits
    * each share between the part that scales f0 and the part added to it.
    */
    Vec2 integrateTexel(float nDotV, float alpha, std::uint32_t samples) {
        const Vec3 toCamera{std::sqrt(1.0f - nDotV * nDotV), nDotV, 0.0f};
        const float alphaSquared = alpha * alpha;
        double scale = 0.0;
        double bias = 0.0;

        for (std::uint32_t sample = 0; sample < samples; ++sample) {
            const float u = (static_cast<float>(sample) + 0.5f) / static_cast<float>(samples);
            const float angle = 2.0f * pi * radicalInverse(sample);
            const float cosineSquared = (1.0f - u) / (1.0f + (alphaSquared - 1.0f) * u);
            const float cosine = std::sqrt(cosineSquared);
            const float sine = std::sqrt(std::max(1.0f - cosineSquared, 0.0f));
            const Vec3 halfway{sine * std::cos(angle), cosine, sine * std::sin(angle)};

            const float vDotH = toCamera.dot(halfway);
            const Vec3 toLight = halfway * (2.0f * vDotH) - toCamera;
            const float nDotL = toLight.y;

            if (nDotL <= 0.0f || vDotH <= 0.0f) {
                continue;
            }

            const float share = 4.0f * smithVisibility(nDotV, nDotL, alpha) * nDotL * vDotH / cosine;
            const float grazing = std::pow(1.0f - vDotH, 5.0f);
            scale += (1.0f - grazing) * share;
            bias += grazing * share;
        }

        return Vec2{static_cast<float>(scale / samples), static_cast<float>(bias / samples)};
    }
}

float clampRoughness(float roughness) {
    return std::clamp(roughness, smallestRoughness, 1.0f);
}

float ggxAlpha(float roughness) {
    const float clamped = clampRoughness(roughness);
    return clamped * clamped;
}

float ggxDistribution(float nDotH, float alpha) {
    const float alphaSquared = alpha * alpha;
    const float denominator = nDotH * nDotH * (alphaSquared - 1.0f) + 1.0f;
    return alphaSquared / (pi * denominator * denominator);
}

/*
* Heitz 2014, eq. 99: G2 = 1 / (1 + Lambda(v) + Lambda(l)) for GGX, which counts a facet hidden from the light and from
* the eye once rather than twice (tall bumps hide from both together), divided by 4 (n.l)(n.v). This form has the
* divisions folded in, so it stays finite at grazing angles.
*/
float smithVisibility(float nDotV, float nDotL, float alpha) {
    const float alphaSquared = alpha * alpha;
    const float towardsView = nDotL * std::sqrt(nDotV * nDotV * (1.0f - alphaSquared) + alphaSquared);
    const float towardsLight = nDotV * std::sqrt(nDotL * nDotL * (1.0f - alphaSquared) + alphaSquared);
    return 0.5f / (towardsView + towardsLight);
}

Vec3 schlickFresnel(const Vec3& f0, float cosine) {
    const float grazing = std::pow(1.0f - std::clamp(cosine, 0.0f, 1.0f), 5.0f);
    return f0 + (Vec3{1.0f, 1.0f, 1.0f} - f0) * grazing;
}

Vec3 reflectanceAtNormal(const SurfaceProperties& surface) {
    const Vec3 dielectric{dielectricReflectance, dielectricReflectance, dielectricReflectance};
    return dielectric + (surface.baseColour - dielectric) * surface.metallic;
}

Vec3 specularBrdf(const Vec3& f0, float alpha, const Vec3& normal, const Vec3& toLight, const Vec3& toCamera) {
    const float nDotL = normal.dot(toLight);
    const float nDotV = normal.dot(toCamera);

    if (nDotL <= 0.0f || nDotV <= 0.0f) {
        return Vec3{};
    }

    const Vec3 halfway = (toLight + toCamera).normalized();
    return specularFromCosines(f0, alpha, nDotV, nDotL, std::max(normal.dot(halfway), 0.0f), std::max(toCamera.dot(halfway), 0.0f));
}

BrdfTable buildBrdfTable(std::uint32_t size, std::uint32_t samples) {
    if (size < 2 || samples == 0) {
        throw std::invalid_argument("A BRDF table needs at least two texels per side and one sample");
    }

    BrdfTable table;
    table.size = size;
    table.entries.resize(static_cast<std::size_t>(size) * size);
    const float last = static_cast<float>(size - 1);

    for (std::uint32_t row = 0; row < size; ++row) {
        const float alpha = ggxAlpha(static_cast<float>(row) / last);

        for (std::uint32_t column = 0; column < size; ++column) {
            const float nDotV = std::max(static_cast<float>(column) / last, smallestViewCosine);
            table.entries[static_cast<std::size_t>(row) * size + column] = integrateTexel(nDotV, alpha, samples);
        }
    }

    return table;
}

/*
* Texel i holds the value at i / (size - 1). The shader reads it with a linear sampler, which places texel i at texture
* coordinate (i + 0.5) / size, so it asks for (value * (size - 1) + 0.5) / size; both blend the two nearest texels
* along each axis.
*/
Vec2 sampleBrdfTable(const BrdfTable& table, float nDotV, float roughness) {
    const float last = static_cast<float>(table.size - 1);
    const float x = std::clamp(nDotV, 0.0f, 1.0f) * last;
    const float y = std::clamp(roughness, 0.0f, 1.0f) * last;
    const std::uint32_t left = static_cast<std::uint32_t>(x);
    const std::uint32_t top = static_cast<std::uint32_t>(y);
    const std::uint32_t right = std::min(left + 1, table.size - 1);
    const std::uint32_t bottom = std::min(top + 1, table.size - 1);
    const float across = x - static_cast<float>(left);
    const float down = y - static_cast<float>(top);

    const auto entry = [&](std::uint32_t column, std::uint32_t row) { return table.entries[static_cast<std::size_t>(row) * table.size + column]; };
    const Vec2 upper = entry(left, top) * (1.0f - across) + entry(right, top) * across;
    const Vec2 lower = entry(left, bottom) * (1.0f - across) + entry(right, bottom) * across;
    return upper * (1.0f - down) + lower * down;
}

ShadingPoint prepareShading(const SurfaceProperties& surface, const Vec3& normal, const Vec3& toCamera, const BrdfTable& table) {
    ShadingPoint point;
    point.normal = normal;
    point.toCamera = toCamera;
    point.nDotV = std::clamp(normal.dot(toCamera), smallestViewCosine, 1.0f);

    const float roughness = clampRoughness(surface.roughness);
    point.alpha = roughness * roughness;
    point.f0 = reflectanceAtNormal(surface);

    const Vec2 entry = sampleBrdfTable(table, point.nDotV, roughness);
    const float facetAlbedo = entry.x + entry.y;
    const Vec3 white{1.0f, 1.0f, 1.0f};

    point.energyCompensation = white + point.f0 * (1.0f / facetAlbedo - 1.0f);
    point.specularAlbedo = (point.f0 * entry.x + Vec3{entry.y, entry.y, entry.y}) * point.energyCompensation;
    point.diffuse = surface.baseColour * (1.0f - surface.metallic) * (white - point.specularAlbedo);

    return point;
}

bool facesLight(const ShadingPoint& point, const Vec3& toLight) {
    return point.normal.dot(toLight) > 0.0f;
}

LightResponse shadeLight(const ShadingPoint& point, const Vec3& toLight, const Vec3& radiance) {
    if (!facesLight(point, toLight)) {
        return LightResponse{};
    }

    const float nDotL = point.normal.dot(toLight);

    LightResponse response;
    response.diffuse = point.diffuse * radiance * nDotL;

    const Vec3 sum = toLight + point.toCamera;
    const float sumLength = sum.length();

    if (sumLength > 0.0f) {
        const Vec3 halfway = sum / sumLength;
        const Vec3 brdf = specularFromCosines(
            point.f0, point.alpha, point.nDotV, nDotL, std::max(point.normal.dot(halfway), 0.0f), std::max(point.toCamera.dot(halfway), 0.0f)
        );
        response.specular = brdf * point.energyCompensation * radiance * (pi * nDotL);
    }

    return response;
}

Vec3 shadeAmbient(const ShadingPoint& point, const Vec3& ambient) {
    return ambient * (point.diffuse + point.specularAlbedo);
}

/*
* A 16-bit UNORM channel stores round(value * 65535): steps of 1.5e-5, far finer than the table's own accuracy, at half
* the size of 32-bit floats. Values are clamped to 0..1 first (the integration can stray just past 1 on a smooth surface).
*/
std::vector<std::uint16_t> brdfTableTexels(const BrdfTable& table) {
    std::vector<std::uint16_t> texels;
    texels.reserve(table.entries.size() * 2);

    const auto encode = [](float value) {
        return static_cast<std::uint16_t>(std::lround(std::clamp(value, 0.0f, 1.0f) * 65535.0f));
    };

    for (const Vec2& entry : table.entries) {
        texels.push_back(encode(entry.x));
        texels.push_back(encode(entry.y));
    }

    return texels;
}
