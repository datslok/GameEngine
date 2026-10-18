#include "render/gpu/brdf.h"

#include <cassert>
#include <cmath>
#include <numbers>
#include <stdexcept>

namespace {
    constexpr double pi = std::numbers::pi;

    bool near(float a, float b, float tolerance) {
        return std::fabs(a - b) <= tolerance;
    }

    bool nearVec(const Vec3& a, const Vec3& b, float tolerance) {
        return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
    }

    // The table is shared by the tests that need one: building it integrates a thousand directions per texel.
    const BrdfTable& table() {
        static const BrdfTable built = buildBrdfTable();
        return built;
    }

    // The eye's direction at the given cosine from the normal (0, 1, 0), tilted towards +x.
    Vec3 viewAt(float cosine) {
        return Vec3{std::sqrt(1.0f - cosine * cosine), cosine, 0.0f};
    }

    const Vec3 up{0.0f, 1.0f, 0.0f};

    /*
    * The share of uniform light from the whole sky that a surface sends towards the eye: the integral over light
    * directions of f cos, with f the shader's BRDF (shadeLight returns pi f cos for a light of 1, hence the 1 / pi).
    * The midpoint rule over cos(theta) and phi, where the solid angle element is d(cos theta) d(phi).
    */
    Vec3 furnace(const SurfaceProperties& surface, float viewCosine, bool specularOnly = false) {
        const ShadingPoint point = prepareShading(surface, up, viewAt(viewCosine), table());
        constexpr int cosineSteps = 400;
        constexpr int angleSteps = 400;
        double sum[3] = {0.0, 0.0, 0.0};

        for (int i = 0; i < cosineSteps; ++i) {
            const double cosine = (i + 0.5) / cosineSteps;
            const double sine = std::sqrt(1.0 - cosine * cosine);

            for (int j = 0; j < angleSteps; ++j) {
                const double angle = 2.0 * pi * (j + 0.5) / angleSteps;
                const Vec3 toLight{static_cast<float>(sine * std::cos(angle)), static_cast<float>(cosine), static_cast<float>(sine * std::sin(angle))};
                const LightResponse response = shadeLight(point, toLight, Vec3{1.0f, 1.0f, 1.0f});
                const Vec3 reflected = specularOnly ? response.specular : response.diffuse + response.specular;
                sum[0] += reflected.x;
                sum[1] += reflected.y;
                sum[2] += reflected.z;
            }
        }

        const double element = (1.0 / cosineSteps) * (2.0 * pi / angleSteps) / pi;
        return Vec3{static_cast<float>(sum[0] * element), static_cast<float>(sum[1] * element), static_cast<float>(sum[2] * element)};
    }

    // A perfect mirror cannot be drawn under a point light, so roughness stops at a small value; alpha is its square.
    void testRoughnessIsClampedAndSquared() {
        assert(near(clampRoughness(0.0f), smallestRoughness, 1e-7f));
        assert(near(clampRoughness(-1.0f), smallestRoughness, 1e-7f));
        assert(near(clampRoughness(2.0f), 1.0f, 1e-7f));
        assert(near(clampRoughness(0.5f), 0.5f, 1e-7f));
        assert(near(ggxAlpha(0.5f), 0.25f, 1e-7f));
        assert(near(ggxAlpha(0.0f), smallestRoughness * smallestRoughness, 1e-7f));
    }

    // Every surface reflects f0 head-on and all light at grazing incidence (a lake at sunset).
    void testFresnelLimits() {
        const Vec3 gold{1.0f, 0.78f, 0.34f};

        assert(nearVec(schlickFresnel(gold, 1.0f), gold, 1e-6f));
        assert(nearVec(schlickFresnel(gold, 0.0f), Vec3{1.0f, 1.0f, 1.0f}, 1e-6f));

        // Rising steadily from one to the other.
        float previous = schlickFresnel(Vec3{0.04f, 0.04f, 0.04f}, 1.0f).x;

        for (float cosine = 0.9f; cosine >= 0.0f; cosine -= 0.1f) {
            const float next = schlickFresnel(Vec3{0.04f, 0.04f, 0.04f}, cosine).x;
            assert(next >= previous);
            previous = next;
        }
    }

    // Non-metals reflect 4% head-on whatever their colour; metals reflect their colour; metallic blends the two.
    void testReflectanceAtNormal() {
        SurfaceProperties red;
        red.baseColour = Vec3{1.0f, 0.0f, 0.0f};

        assert(nearVec(reflectanceAtNormal(red), Vec3{0.04f, 0.04f, 0.04f}, 1e-6f));

        red.metallic = 1.0f;
        assert(nearVec(reflectanceAtNormal(red), Vec3{1.0f, 0.0f, 0.0f}, 1e-6f));

        red.metallic = 0.5f;
        assert(nearVec(reflectanceAtNormal(red), Vec3{0.52f, 0.02f, 0.02f}, 1e-6f));
    }

    /*
    * GGX is a density of facet normals: their area projected onto the surface must equal the surface's own area, so
    * the integral of D (n.h) over the hemisphere is 1 for every roughness. D depends only on the angle, so the integral
    * over the azimuth is 2 pi and one dimension is left.
    */
    void testGgxIsNormalised() {
        for (float roughness : {0.1f, 0.3f, 0.5f, 0.8f, 1.0f}) {
            const float alpha = ggxAlpha(roughness);
            constexpr int steps = 400000;
            double sum = 0.0;

            for (int i = 0; i < steps; ++i) {
                const double cosine = (i + 0.5) / steps;
                sum += ggxDistribution(static_cast<float>(cosine), alpha) * cosine;
            }

            assert(near(static_cast<float>(sum * 2.0 * pi / steps), 1.0f, 0.005f));
        }
    }

    // G = 4 V (n.l)(n.v) is a share of facets, so 0..1; nothing is blocked on a smooth surface; light and eye can swap.
    void testSmithVisibility() {
        for (float alpha : {0.01f, 0.1f, 0.5f, 1.0f}) {
            for (float nDotV : {0.05f, 0.3f, 0.7f, 1.0f}) {
                for (float nDotL : {0.05f, 0.5f, 1.0f}) {
                    const float visibility = smithVisibility(nDotV, nDotL, alpha);
                    const float shadowing = 4.0f * visibility * nDotV * nDotL;

                    assert(shadowing > 0.0f && shadowing <= 1.0f + 1e-5f);
                    assert(near(visibility, smithVisibility(nDotL, nDotV, alpha), 1e-6f));
                }
            }
        }

        assert(near(4.0f * smithVisibility(0.5f, 0.5f, 1e-4f) * 0.25f, 1.0f, 1e-3f));

        // Rougher surfaces hide more of themselves at a glancing view.
        assert(smithVisibility(0.2f, 0.8f, 0.8f) < smithVisibility(0.2f, 0.8f, 0.2f));
    }

    // Helmholtz reciprocity: light follows the same path backwards, so swapping the light and the eye changes nothing.
    void testSpecularIsReciprocal() {
        const Vec3 f0{0.9f, 0.6f, 0.3f};
        const Vec3 first = Vec3{0.3f, 0.8f, 0.1f}.normalized();
        const Vec3 second = Vec3{-0.5f, 0.6f, 0.4f}.normalized();

        for (float roughness : {0.2f, 0.6f, 1.0f}) {
            const float alpha = ggxAlpha(roughness);
            assert(nearVec(specularBrdf(f0, alpha, up, first, second), specularBrdf(f0, alpha, up, second, first), 1e-5f));
        }

        // Nothing from below the surface or towards an eye below it.
        assert(nearVec(specularBrdf(f0, 0.25f, up, Vec3{0.0f, -1.0f, 0.0f}, up), Vec3{}, 0.0f));
        assert(nearVec(specularBrdf(f0, 0.25f, up, up, Vec3{0.0f, -1.0f, 0.0f}), Vec3{}, 0.0f));
    }

    // The table has one scale and bias per texel, a mirror's facets reflect at most everything, and the built table
    // matches a brute-force integration of D V F cos over every light direction.
    void testBrdfTable() {
        const BrdfTable& built = table();
        assert(built.size == brdfTableSize);
        assert(built.entries.size() == static_cast<std::size_t>(brdfTableSize) * brdfTableSize);

        for (const Vec2& entry : built.entries) {
            assert(entry.x >= 0.0f && entry.y >= 0.0f);
            assert(entry.x + entry.y <= 1.0f + 0.01f);
        }

        for (float roughness : {0.3f, 0.6f, 1.0f}) {
            for (float viewCosine : {0.2f, 0.5f, 0.9f}) {
                SurfaceProperties surface;
                surface.roughness = roughness;
                const ShadingPoint point = prepareShading(surface, up, viewAt(viewCosine), built);
                const Vec2 entry = sampleBrdfTable(built, viewCosine, roughness);

                // Single scattering, with a white mirror (f0 = 1) and a grey one (f0 = 0.04), integrated directly.
                for (float f0 : {1.0f, 0.04f}) {
                    constexpr int steps = 500;
                    double sum = 0.0;

                    for (int i = 0; i < steps; ++i) {
                        const double cosine = (i + 0.5) / steps;
                        const double sine = std::sqrt(1.0 - cosine * cosine);

                        for (int j = 0; j < steps; ++j) {
                            const double angle = 2.0 * pi * (j + 0.5) / steps;
                            const Vec3 toLight{static_cast<float>(sine * std::cos(angle)), static_cast<float>(cosine), static_cast<float>(sine * std::sin(angle))};
                            sum += specularBrdf(Vec3{f0, f0, f0}, point.alpha, up, toLight, point.toCamera).x * cosine;
                        }
                    }

                    const float integrated = static_cast<float>(sum * (1.0 / steps) * (2.0 * pi / steps));
                    assert(near(f0 * entry.x + entry.y, integrated, 0.01f));
                }
            }
        }

        // A nearly smooth mirror loses (almost) nothing; a rough one loses a lot to light bouncing between its facets.
        const Vec2 smooth = sampleBrdfTable(built, 0.7f, smallestRoughness);
        assert(smooth.x + smooth.y > 0.97f);

        const Vec2 rough = sampleBrdfTable(built, 0.7f, 1.0f);
        assert(rough.x + rough.y < 0.9f);

        // The rougher, the more is lost.
        float previous = 2.0f;

        for (float roughness = 0.1f; roughness <= 1.0f; roughness += 0.1f) {
            const Vec2 entry = sampleBrdfTable(built, 0.5f, roughness);
            assert(entry.x + entry.y <= previous + 1e-4f);
            previous = entry.x + entry.y;
        }
    }

    // The table's corners are 0 and 1 exactly; reading at a grid point gives that texel; between them it blends; past
    // the edges it clamps.
    void testBrdfTableSampling() {
        BrdfTable small;
        small.size = 3;
        small.entries = {
            Vec2{0.0f, 0.0f}, Vec2{1.0f, 0.0f}, Vec2{2.0f, 0.0f},
            Vec2{0.0f, 1.0f}, Vec2{1.0f, 1.0f}, Vec2{2.0f, 1.0f},
            Vec2{0.0f, 2.0f}, Vec2{1.0f, 2.0f}, Vec2{2.0f, 2.0f}
        };

        assert(near(sampleBrdfTable(small, 0.0f, 0.0f).x, 0.0f, 1e-6f));
        assert(near(sampleBrdfTable(small, 0.5f, 0.0f).x, 1.0f, 1e-6f));
        assert(near(sampleBrdfTable(small, 1.0f, 0.0f).x, 2.0f, 1e-6f));
        assert(near(sampleBrdfTable(small, 0.0f, 1.0f).y, 2.0f, 1e-6f));
        assert(near(sampleBrdfTable(small, 0.25f, 0.75f).x, 0.5f, 1e-6f));
        assert(near(sampleBrdfTable(small, 0.25f, 0.75f).y, 1.5f, 1e-6f));
        assert(near(sampleBrdfTable(small, 2.0f, -1.0f).x, 2.0f, 1e-6f));
        assert(near(sampleBrdfTable(small, 2.0f, -1.0f).y, 0.0f, 1e-6f));
    }

    // The GPU copy keeps each number to within half a 16-bit step, in the same order, clamped to 0..1.
    void testBrdfTableTexels() {
        BrdfTable small;
        small.size = 2;
        small.entries = {Vec2{0.0f, 1.0f}, Vec2{0.5f, 0.25f}, Vec2{1.2f, -0.1f}, Vec2{0.123f, 0.987f}};

        const std::vector<std::uint16_t> texels = brdfTableTexels(small);
        assert(texels.size() == 8);
        assert(texels[0] == 0 && texels[1] == 65535);
        assert(near(texels[2] / 65535.0f, 0.5f, 1e-5f) && near(texels[3] / 65535.0f, 0.25f, 1e-5f));
        assert(texels[4] == 65535 && texels[5] == 0);
        assert(near(texels[6] / 65535.0f, 0.123f, 1e-5f) && near(texels[7] / 65535.0f, 0.987f, 1e-5f));
    }

    void testBrdfTableRejectsEmpty() {
        bool threw = false;

        try {
            buildBrdfTable(1, 16);
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
        threw = false;

        try {
            buildBrdfTable(4, 0);
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
    }

    /*
    * The white furnace test: a surface that absorbs nothing, under the same light from every direction, must send back
    * exactly the light it receives, at every roughness and from every view. With the multiple-scattering compensation
    * a white metal (all mirror) and a white non-metal (mirror plus diffuse underneath) both pass.
    */
    void testWhiteFurnace() {
        for (float roughness : {0.3f, 0.6f, 1.0f}) {
            for (float viewCosine : {0.2f, 0.5f, 0.9f}) {
                SurfaceProperties metal;
                metal.metallic = 1.0f;
                metal.roughness = roughness;
                assert(nearVec(furnace(metal, viewCosine), Vec3{1.0f, 1.0f, 1.0f}, 0.02f));

                SurfaceProperties plastic;
                plastic.roughness = roughness;
                assert(nearVec(furnace(plastic, viewCosine), Vec3{1.0f, 1.0f, 1.0f}, 0.02f));
            }
        }
    }

    // Without the compensation a rough mirror visibly loses light; with it, what it reflects is the table's prediction.
    void testCompensationRestoresLostLight() {
        SurfaceProperties gold;
        gold.baseColour = Vec3{1.0f, 0.78f, 0.34f};
        gold.metallic = 1.0f;
        gold.roughness = 1.0f;

        const ShadingPoint point = prepareShading(gold, up, viewAt(0.5f), table());
        const Vec2 entry = sampleBrdfTable(table(), 0.5f, 1.0f);
        const Vec3 singleScattering = point.f0 * entry.x + Vec3{entry.y, entry.y, entry.y};

        assert(point.energyCompensation.x > 1.05f);
        assert(nearVec(point.specularAlbedo, singleScattering * point.energyCompensation, 1e-5f));
        assert(nearVec(furnace(gold, 0.5f, true), point.specularAlbedo, 0.02f));

        // Coloured metal reflects less than white, and more than the single-scattering model alone.
        for (int channel = 0; channel < 3; ++channel) {
            assert(point.specularAlbedo[channel] <= 1.0f + 1e-5f);
            assert(point.specularAlbedo[channel] > singleScattering[channel]);
        }
    }

    // Metals have no diffuse part; their reflection takes their colour, so a red metal's highlight is red.
    void testMetalsReflectTheirColour() {
        SurfaceProperties redMetal;
        redMetal.baseColour = Vec3{1.0f, 0.1f, 0.1f};
        redMetal.metallic = 1.0f;
        redMetal.roughness = 0.4f;

        const ShadingPoint point = prepareShading(redMetal, up, viewAt(0.8f), table());
        assert(nearVec(point.diffuse, Vec3{}, 0.0f));

        const Vec3 mirrored{-viewAt(0.8f).x, viewAt(0.8f).y, 0.0f};
        const LightResponse response = shadeLight(point, mirrored, Vec3{1.0f, 1.0f, 1.0f});

        assert(nearVec(response.diffuse, Vec3{}, 0.0f));
        assert(response.specular.x > 5.0f * response.specular.y);
    }

    /*
    * The unit convention: diffuse light is the weighted diffuse colour times the light times the cosine, with no 1 / pi,
    * as before physically based shading, so the demo's lights keep their brightness.
    */
    void testDiffuseKeepsTheLightUnits() {
        SurfaceProperties grey;
        grey.baseColour = Vec3{0.5f, 0.5f, 0.5f};
        grey.roughness = 1.0f;

        const ShadingPoint point = prepareShading(grey, up, viewAt(1.0f), table());
        const Vec3 toLight = Vec3{0.6f, 0.8f, 0.0f};
        const LightResponse response = shadeLight(point, toLight, Vec3{2.0f, 2.0f, 2.0f});

        assert(nearVec(response.diffuse, point.diffuse * 2.0f * 0.8f, 1e-6f));

        // The diffuse part is what the specular part leaves of the colour.
        assert(nearVec(point.diffuse, Vec3{0.5f, 0.5f, 0.5f} * (Vec3{1.0f, 1.0f, 1.0f} - point.specularAlbedo), 1e-6f));
        assert(point.diffuse.x < 0.5f && point.diffuse.x > 0.45f);
    }

    // Nothing from a light behind the surface.
    void testLightBehindGivesNothing() {
        const ShadingPoint point = prepareShading(SurfaceProperties{}, up, viewAt(0.7f), table());
        const LightResponse response = shadeLight(point, Vec3{0.0f, -1.0f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});

        assert(nearVec(response.diffuse, Vec3{}, 0.0f));
        assert(nearVec(response.specular, Vec3{}, 0.0f));
    }

    /*
    * Ambient light is the same from every direction, so what a surface sends back is its albedo: a white non-metal
    * returns all of it, and a metal, which has no diffuse part, still reflects its colour instead of turning black.
    */
    void testAmbient() {
        const Vec3 white{1.0f, 1.0f, 1.0f};
        const ShadingPoint plastic = prepareShading(SurfaceProperties{}, up, viewAt(0.6f), table());
        assert(nearVec(shadeAmbient(plastic, white), white, 1e-5f));

        SurfaceProperties gold;
        gold.baseColour = Vec3{1.0f, 0.78f, 0.34f};
        gold.metallic = 1.0f;
        const ShadingPoint metal = prepareShading(gold, up, viewAt(0.6f), table());
        const Vec3 reflected = shadeAmbient(metal, Vec3{0.5f, 0.5f, 0.5f});

        assert(nearVec(reflected, metal.specularAlbedo * 0.5f, 1e-6f));
        assert(reflected.z > 0.1f && reflected.x > reflected.z);
    }

    // An eye exactly along the surface (n.v = 0) or a little below it (a normal interpolated across a curved mesh)
    // still gives finite light.
    void testGrazingViewStaysFinite() {
        for (float viewCosine : {0.0f, -0.2f}) {
            const Vec3 toCamera = Vec3{1.0f, viewCosine, 0.0f}.normalized();
            const ShadingPoint point = prepareShading(SurfaceProperties{}, up, toCamera, table());
            const LightResponse response = shadeLight(point, Vec3{-0.6f, 0.8f, 0.0f}, Vec3{1.0f, 1.0f, 1.0f});

            for (int channel = 0; channel < 3; ++channel) {
                assert(std::isfinite(response.diffuse[channel]) && std::isfinite(response.specular[channel]));
                assert(std::isfinite(point.specularAlbedo[channel]));
            }
        }
    }
}

void testBrdf() {
    testRoughnessIsClampedAndSquared();
    testFresnelLimits();
    testReflectanceAtNormal();
    testGgxIsNormalised();
    testSmithVisibility();
    testSpecularIsReciprocal();
    testBrdfTableSampling();
    testBrdfTableRejectsEmpty();
    testBrdfTableTexels();
    testBrdfTable();
    testWhiteFurnace();
    testCompensationRestoresLostLight();
    testMetalsReflectTheirColour();
    testDiffuseKeepsTheLightUnits();
    testLightBehindGivesNothing();
    testAmbient();
    testGrazingViewStaysFinite();
}
