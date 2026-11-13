#include "render/gpu/brdf.h"
#include "core/job_system.h"

#include <cassert>
#include <cmath>
#include <cstring>
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
        static const BrdfTable built = [] {
            JobSystem jobs;
            return buildBrdfTable(jobs);
        }();
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
    Vec3 furnace(const SurfaceProperties& surface, float viewCosine, bool specularOnly = false, const ClearCoatProperties& coat = {}) {
        ShadingPoint point = prepareShading(surface, up, viewAt(viewCosine), table());
        addClearCoat(point, coat, table());
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

    // Built on many threads, the table is the same to the last bit as on one: each texel is integrated on its own.
    void testBrdfTableOnManyThreads() {
        JobSystem serial{0};
        JobSystem parallel{7};
        const BrdfTable one = buildBrdfTable(serial, 16, 256);
        const BrdfTable many = buildBrdfTable(parallel, 16, 256);
        assert(one.size == many.size);
        assert(one.entries.size() == many.entries.size());
        assert(std::memcmp(one.entries.data(), many.entries.data(), one.entries.size() * sizeof(Vec2)) == 0);
    }

    void testBrdfTableRejectsEmpty() {
        JobSystem jobs{0};
        bool threw = false;

        try {
            buildBrdfTable(jobs, 1, 16);
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
        threw = false;

        try {
            buildBrdfTable(jobs, 4, 0);
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

    // The matte colour rays' hits are lit with: a metal has none, a half-metal half its base colour, a non-metal all of it.
    void testMatteColour() {
        const Vec3 base{0.8f, 0.6f, 0.4f};
        assert(nearVec(matteColour(base, 1.0f), Vec3{}, 0.0f));
        assert(nearVec(matteColour(base, 0.5f), base * 0.5f, 1e-7f));
        assert(nearVec(matteColour(base, 0.0f), base, 0.0f));
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
    * A light reaches the surface only from in front of it: grazing along the surface (a cosine of exactly 0) or from
    * behind gives nothing. The shader asks this before a light's shadow lookups, which a light giving nothing anyway
    * would waste; shadeLight asks the same, so the two can never disagree.
    */
    void testFacesLightOnlyFromInFront() {
        const ShadingPoint point = prepareShading(SurfaceProperties{}, up, viewAt(0.7f), table());
        assert(facesLight(point, Vec3{0.6f, 0.8f, 0.0f}));
        assert(facesLight(point, up));
        assert(!facesLight(point, Vec3{1.0f, 0.0f, 0.0f}));
        assert(!facesLight(point, Vec3{0.0f, -1.0f, 0.0f}));
        assert(!facesLight(point, Vec3{0.6f, -0.8f, 0.0f}));
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

namespace {
    const Vec3 white{1.0f, 1.0f, 1.0f};

    bool sameVec(const Vec3& a, const Vec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    }

    /*
    * A clear coat of nothing changes nothing, to the last bit: the shading point, every light's response and the
    * ambient are exactly what they were without one, so uncoated materials draw as before.
    */
    void testNoCoatChangesNothing() {
        SurfaceProperties paint;
        paint.baseColour = Vec3{0.7f, 0.1f, 0.1f};
        paint.roughness = 0.4f;
        const ShadingPoint plain = prepareShading(paint, up, viewAt(0.6f), table());
        ShadingPoint coated = plain;
        addClearCoat(coated, ClearCoatProperties{}, table());

        assert(coated.coat == 0.0f && coated.coatAlbedo == 0.0f);
        assert(sameVec(coated.diffuse, plain.diffuse) && sameVec(coated.specularAlbedo, plain.specularAlbedo));
        assert(sameVec(coated.energyCompensation, plain.energyCompensation));

        const Vec3 toLight = Vec3{-0.5f, 0.7f, 0.2f}.normalized();
        const LightResponse before = shadeLight(plain, toLight, white);
        const LightResponse after = shadeLight(coated, toLight, white);
        assert(sameVec(before.diffuse, after.diffuse) && sameVec(before.specular, after.specular));
        assert(sameVec(shadeAmbient(plain, white), shadeAmbient(coated, white)));
        assert(sameVec(shadeProbeAmbient(plain, white * 0.3f, white * 0.7f), shadeProbeAmbient(coated, white * 0.3f, white * 0.7f)));
        assert(!reflectsCoat(coated));
    }

    /*
    * The white furnace test with a clear coat: what the coat reflects is exactly what the layer under it does not
    * receive, so a white surface under even light still returns all of it, coated fully or partly, smooth or rough,
    * over a metal or a non-metal, from every view.
    */
    void testCoatedWhiteFurnace() {
        for (float amount : {0.5f, 1.0f}) {
            for (float coatRoughness : {0.3f, 0.6f}) {
                for (float viewCosine : {0.2f, 0.5f, 0.9f}) {
                    const ClearCoatProperties coat{.amount = amount, .roughness = coatRoughness, .normal = up};

                    SurfaceProperties metal;
                    metal.metallic = 1.0f;
                    metal.roughness = 0.5f;
                    assert(nearVec(furnace(metal, viewCosine, false, coat), white, 0.02f));

                    SurfaceProperties plastic;
                    plastic.roughness = 0.7f;
                    assert(nearVec(furnace(plastic, viewCosine, false, coat), white, 0.02f));
                }
            }
        }
    }

    /*
    * The coat's own share: the table's albedo for a 4% reflector at the coat's roughness and view, with the same
    * compensation for light lost between facets, times the coat's amount; the base keeps the rest of what it had. It
    * rises towards grazing views (Fresnel), where the base dims to match; under even light the coat's lobe alone
    * returns exactly its albedo.
    */
    void testCoatShare() {
        SurfaceProperties paint;
        paint.baseColour = Vec3{0.1f, 0.3f, 0.8f};
        paint.roughness = 0.6f;
        const ClearCoatProperties coat{.amount = 0.8f, .roughness = 0.3f, .normal = up};

        const auto coatedAt = [&](float viewCosine) {
            ShadingPoint point = prepareShading(paint, up, viewAt(viewCosine), table());
            addClearCoat(point, coat, table());
            return point;
        };

        const ShadingPoint point = coatedAt(0.7f);
        const Vec2 entry = sampleBrdfTable(table(), 0.7f, 0.3f);
        const float facets = entry.x + entry.y;
        const float compensation = 1.0f + dielectricReflectance * (1.0f / facets - 1.0f);
        assert(near(point.coatAlbedo, 0.8f * (dielectricReflectance * entry.x + entry.y) * compensation, 1e-6f));

        const ShadingPoint plain = prepareShading(paint, up, viewAt(0.7f), table());
        const float baseShare = 1.0f - point.coatAlbedo;
        assert(nearVec(point.diffuse, plain.diffuse * baseShare, 1e-6f));
        assert(nearVec(point.specularAlbedo, plain.specularAlbedo * baseShare, 1e-6f));

        const ShadingPoint grazing = coatedAt(0.15f);
        const ShadingPoint headOn = coatedAt(0.95f);
        assert(grazing.coatAlbedo > 2.0f * headOn.coatAlbedo);
        assert(headOn.coatAlbedo > 0.8f * dielectricReflectance * 0.9f && headOn.coatAlbedo < 0.8f * 0.06f);

        // Under even light from everywhere the coat's lobe alone (over a black base that reflects nothing) returns its albedo.
        SurfaceProperties black;
        black.baseColour = Vec3{};
        ShadingPoint blackCoated = prepareShading(black, up, viewAt(0.7f), table());
        addClearCoat(blackCoated, coat, table());
        const Vec3 baseAlbedo = prepareShading(black, up, viewAt(0.7f), table()).specularAlbedo;
        const Vec3 expected = baseAlbedo * (1.0f - blackCoated.coatAlbedo) + white * blackCoated.coatAlbedo;
        assert(nearVec(furnace(black, 0.7f, true, coat), expected, 0.01f));

        // Ambient light: the coat's albedo is added to the base's, weighted by what the coat let through.
        assert(nearVec(shadeAmbient(point, white), point.diffuse + point.specularAlbedo + white * point.coatAlbedo, 1e-6f));
        assert(nearVec(shadeProbeAmbient(point, white * 0.5f, white * 2.0f),
                       point.diffuse * 0.5f + (point.specularAlbedo + white * point.coatAlbedo) * 2.0f, 1e-6f));
    }

    /*
    * The coat lies smooth on the geometry, whatever the normal map does to the layer under it: its highlight is where
    * the light mirrors about the coat's own normal. A rough base tilted by a normal map, coated with smooth varnish, shows
    * a sharp highlight about the coat's normal that the uncoated surface lacks; the coat's highlight is white, whatever
    * the base's colour.
    */
    void testCoatIgnoresTheNormalMap() {
        SurfaceProperties wood;
        wood.baseColour = Vec3{0.5f, 0.3f, 0.1f};
        wood.roughness = 0.9f;

        const Vec3 bumped = Vec3{0.4f, 0.9f, 0.0f}.normalized();
        const Vec3 toCamera = viewAt(0.8f);
        const Vec3 mirrored{-toCamera.x, toCamera.y, 0.0f};

        const ShadingPoint plain = prepareShading(wood, bumped, toCamera, table());
        ShadingPoint varnished = plain;
        addClearCoat(varnished, ClearCoatProperties{.amount = 1.0f, .roughness = 0.05f, .normal = up}, table());

        const LightResponse without = shadeLight(plain, mirrored, white);
        const LightResponse with = shadeLight(varnished, mirrored, white);
        assert(with.specular.x > 20.0f * without.specular.x);
        assert(near(with.specular.x - without.specular.x * (1.0f - varnished.coatAlbedo), with.specular.z - without.specular.z * (1.0f - varnished.coatAlbedo), 1e-3f * with.specular.x));

        // Away from the coat's mirror direction its highlight fades: a light mirrored about the bumped normal instead.
        const Vec3 aboutBump = (bumped * (2.0f * bumped.dot(toCamera)) - toCamera).normalized();
        assert(shadeLight(varnished, aboutBump, white).specular.x < 0.1f * with.specular.x);
    }

    /*
    * A light reaches a coated surface if it faces either layer: the coat's normal can face a light the normal map has
    * tilted the base away from, and the coat's highlight is still there.
    */
    void testCoatFacesItsOwnLights() {
        const Vec3 tilted = Vec3{0.8f, 0.6f, 0.0f}.normalized();
        ShadingPoint point = prepareShading(SurfaceProperties{}, tilted, viewAt(0.9f), table());
        const Vec3 toLight = Vec3{-0.7f, 0.3f, 0.0f}.normalized();
        assert(!facesLight(point, toLight));

        addClearCoat(point, ClearCoatProperties{.amount = 1.0f, .roughness = 0.5f, .normal = up}, table());
        assert(facesLight(point, toLight));
        const LightResponse response = shadeLight(point, toLight, white);
        assert(response.specular.x > 0.0f && nearVec(response.diffuse, Vec3{}, 0.0f));

        // From below both layers, nothing.
        assert(!facesLight(point, Vec3{0.0f, -1.0f, 0.0f}));
    }

    /*
    * The reflection pass follows one layer a pixel: the smoother (the coat on a tie), which shows the sharper mirror image;
    * the other keeps its stand-in. No coat, the base. The coat's roughness is clamped as the base's is.
    */
    void testReflectionFollowsTheSmootherLayer() {
        SurfaceProperties paint;
        paint.roughness = 0.5f;
        ShadingPoint carPaint = prepareShading(paint, up, viewAt(0.7f), table());
        addClearCoat(carPaint, ClearCoatProperties{.amount = 1.0f, .roughness = 0.0f, .normal = up}, table());
        assert(reflectsCoat(carPaint));
        assert(carPaint.coatAlpha == ggxAlpha(smallestRoughness));

        SurfaceProperties chrome;
        chrome.metallic = 1.0f;
        chrome.roughness = 0.1f;
        ShadingPoint matteCoated = prepareShading(chrome, up, viewAt(0.7f), table());
        addClearCoat(matteCoated, ClearCoatProperties{.amount = 1.0f, .roughness = 0.6f, .normal = up}, table());
        assert(!reflectsCoat(matteCoated));

        ShadingPoint tie = prepareShading(chrome, up, viewAt(0.7f), table());
        addClearCoat(tie, ClearCoatProperties{.amount = 0.5f, .roughness = 0.1f, .normal = up}, table());
        assert(reflectsCoat(tie));
    }
}

namespace {
    /*
    * GGX's sampled facet normals: unit length, all along the normal for a mirror, spread more the rougher, and evenly
    * round the normal (their sideways parts average out). Turned round another normal, they keep their angle to it.
    */
    void testGgxSamples() {
        assert(radicalInverse(1) == 0.5f && radicalInverse(2) == 0.25f && radicalInverse(3) == 0.75f);

        for (const float alpha : {0.0f, 0.01f, 0.25f, 1.0f}) {
            Vec3 sum;
            float sumCosine = 0.0f;
            constexpr std::uint32_t samples = 256;

            for (std::uint32_t sample = 0; sample < samples; ++sample) {
                const Vec3 halfway = ggxSampleHalfway(sample, samples, alpha);
                assert(near(halfway.length(), 1.0f, 1e-5f) && halfway.y >= 0.0f);
                sum = sum + halfway;
                sumCosine += halfway.y;

                if (alpha == 0.0f) {
                    assert(nearVec(halfway, up, 1e-6f));
                }
            }

            assert(std::fabs(sum.x) / samples < 0.02f && std::fabs(sum.z) / samples < 0.02f);
            assert(alpha < 0.3f || sumCosine / samples < 0.9f);
        }

        const Vec3 normal = Vec3{0.3f, -0.5f, 0.8f}.normalized();
        assert(nearVec(alongNormal(up, normal), normal, 1e-6f));

        for (const Vec3 local : {Vec3{0.6f, 0.8f, 0.0f}, Vec3{0.0f, 0.6f, -0.8f}, Vec3{-0.48f, 0.6f, 0.64f}}) {
            const Vec3 turned = alongNormal(local, normal);
            assert(near(turned.length(), 1.0f, 1e-5f) && near(turned.dot(normal), local.y, 1e-5f));
        }

        // Straight down and up, where the frame has to pick another helper axis.
        assert(nearVec(alongNormal(up, Vec3{0.0f, -1.0f, 0.0f}), Vec3{0.0f, -1.0f, 0.0f}, 1e-6f));
        assert(near(alongNormal(Vec3{0.6f, 0.8f, 0.0f}, up).dot(up), 0.8f, 1e-6f));
    }
}

void testBrdf() {
    testGgxSamples();
    testRoughnessIsClampedAndSquared();
    testFresnelLimits();
    testReflectanceAtNormal();
    testGgxIsNormalised();
    testSmithVisibility();
    testSpecularIsReciprocal();
    testBrdfTableSampling();
    testBrdfTableOnManyThreads();
    testBrdfTableRejectsEmpty();
    testBrdfTableTexels();
    testBrdfTable();
    testWhiteFurnace();
    testCompensationRestoresLostLight();
    testMetalsReflectTheirColour();
    testMatteColour();
    testDiffuseKeepsTheLightUnits();
    testLightBehindGivesNothing();
    testFacesLightOnlyFromInFront();
    testAmbient();
    testGrazingViewStaysFinite();
    testNoCoatChangesNothing();
    testCoatedWhiteFurnace();
    testCoatShare();
    testCoatIgnoresTheNormalMap();
    testCoatFacesItsOwnLights();
    testReflectionFollowsTheSmootherLayer();
}
