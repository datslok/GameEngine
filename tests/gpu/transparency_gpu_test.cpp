#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/transparency.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("transparency_test");

    Vec3 randomColour(CaseRandom& random, float most) {
        return Vec3{random.between(0.0f, most), random.between(0.0f, most), random.between(0.0f, most)};
    }

    /*
    * Coverage, transmission and metallic across 0..1 (each end included), light from dark to bright. Inputs per case:
    * (alpha, transmission, metallic), diffuse light, specular light, emissive, base colour, specular albedo. Outputs:
    * the light added, the transmittance.
    */
    void checkBlend(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{307};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const float alpha = i % 7 == 0 ? 1.0f : (i % 11 == 0 ? 0.0f : random.between(0.0f, 1.0f));
            const float transmission = i % 5 == 0 ? 0.0f : (i % 3 == 0 ? 1.0f : random.between(0.0f, 1.0f));
            inputs.push_back(GpuVec4{alpha, transmission, i % 9 == 0 ? 1.0f : random.between(0.0f, 1.0f), 0.0f});
            inputs.push_back(gpuVec4(randomColour(random, 10.0f), 0.0f));
            inputs.push_back(gpuVec4(randomColour(random, 10.0f), 0.0f));
            inputs.push_back(gpuVec4(randomColour(random, 2.0f), 0.0f));
            inputs.push_back(gpuVec4(randomColour(random, 1.0f), 0.0f));
            inputs.push_back(gpuVec4(randomColour(random, 1.0f), 0.0f));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TRANSPARENT_BLEND, inputs, 6, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 6];
            const TransparentLight expected = transparentBlend(in[0][0], in[0][1], in[0][2], vec3From(in[1]), vec3From(in[2]), vec3From(in[3]),
                                                               vec3From(in[4]), vec3From(in[5]));
            report.check("transparentBlend", arithmeticTolerance, expected.added, vec3From(outputs[i * 2]));
            report.check("transparentBlend", arithmeticTolerance, expected.transmittance, vec3From(outputs[i * 2 + 1]));
        }
    }

    /*
    * The reactive mask's share: pictures from black to very bright, the glass's picture darker, brighter or the same,
    * scales up to 3 and limits across 0..1. Inputs per case: the picture before, the picture after, (scale, limit).
    * Output: x the share.
    */
    void checkReactiveShare(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{311};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const Vec3 before = i % 13 == 0 ? Vec3{0.0f, 0.0f, 0.0f} : randomColour(random, i % 2 == 0 ? 1.0f : 1000.0f);
            const Vec3 after = i % 5 == 0 ? before : before * random.between(0.0f, 3.0f) + randomColour(random, 0.1f);
            inputs.push_back(gpuVec4(before, 0.0f));
            inputs.push_back(gpuVec4(after, 0.0f));
            inputs.push_back(GpuVec4{random.between(0.0f, 3.0f), i % 7 == 0 ? 1.0f : random.between(0.0f, 1.0f), 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_REACTIVE_SHARE, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 3];
            report.check("reactiveShare", arithmeticTolerance, reactiveShare(vec3From(in[0]), vec3From(in[1]), in[2][0], in[2][1]), outputs[i][0]);
        }
    }

    /*
    * Glass on a ray towards a light: the light it lets through at angles from grazing to head-on from either side, and
    * whether segments pass within spheres (hits, near misses, spheres behind and beyond the segment). Inputs per case:
    * (alpha, transmission, metallic, cosine), base colour, the segment's start, (its unit direction, length), (the
    * sphere's centre, radius). Outputs: the transmittance, x 1 when the segment meets the sphere.
    */
    void checkGlassRays(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{313};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const float alpha = i % 7 == 0 ? 1.0f : random.between(0.0f, 1.0f);
            const float transmission = i % 3 == 0 ? 1.0f : random.between(0.0f, 1.0f);
            inputs.push_back(GpuVec4{alpha, transmission, i % 9 == 0 ? 1.0f : random.between(0.0f, 0.2f), random.between(-1.0f, 1.0f)});
            inputs.push_back(gpuVec4(randomColour(random, 1.0f), 0.0f));

            const Vec3 start{random.between(-10.0f, 10.0f), random.between(-10.0f, 10.0f), random.between(-10.0f, 10.0f)};
            const Vec3 direction = Vec3{random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(0.1f, 1.0f)}.normalized();
            const float length = random.between(1.0f, 30.0f);
            const Vec3 centre = start + direction * random.between(-5.0f, length + 5.0f) +
                                Vec3{random.between(-3.0f, 3.0f), random.between(-3.0f, 3.0f), random.between(-3.0f, 3.0f)};
            inputs.push_back(gpuVec4(start, 0.0f));
            inputs.push_back(gpuVec4(direction, length));
            inputs.push_back(gpuVec4(centre, random.between(0.1f, 3.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_GLASS_RAYS, inputs, 5, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 5];
            const Vec3 passing = glassLightTransmittance(in[0][0], in[0][1], in[0][2], vec3From(in[1]), in[0][3]);
            report.check("glassLightTransmittance", transcendentalTolerance, passing, vec3From(outputs[i * 2]));

            // The sphere test is a yes or no: compared only away from the edge, where rounding cannot decide it.
            const Vec3 start = vec3From(in[2]);
            const Vec3 direction = vec3From(in[3]);
            const Vec3 centre = vec3From(in[4]);
            const float along = std::clamp((centre - start).dot(direction), 0.0f, in[3][3]);
            const float gap = (start + direction * along - centre).length() - in[4][3];

            if (std::abs(gap) > 1e-3f) {
                const bool meets = segmentMeetsSphere(start, direction, in[3][3], centre, in[4][3]);
                report.check("segmentMeetsSphere", exactTolerance, meets ? 1.0f : 0.0f, outputs[i * 2 + 1][0]);
            }
        }
    }
}

namespace {
    /*
    * Refraction: rough glass's spread and the blurred picture's level across roughness, index, distances behind and in
    * front, Beer's absorption across colours and lengths, and thick glass's exit point at angles from head-on to grazing.
    * Inputs per case: (roughness, ior, behind, view distance), (pixels per radian, levels, inverse attenuation distance,
    * length), the kept colour, the entry point, the way to the camera, (the normal, the thickness). Outputs: (spread,
    * level), the absorption, the exit point.
    */
    void checkRefraction(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{317};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            inputs.push_back(GpuVec4{random.between(0.0f, 1.0f), random.between(1.0f, 2.5f), random.between(0.0f, 50.0f), random.between(0.1f, 50.0f)});
            inputs.push_back(GpuVec4{random.between(200.0f, 3000.0f), static_cast<float>(1 + i % 12), i % 5 == 0 ? 0.0f : random.between(0.0f, 4.0f),
                                     random.between(0.0f, 3.0f)});
            inputs.push_back(gpuVec4(randomColour(random, 1.0f), 0.0f));

            const Vec3 normal = Vec3{random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f)}.normalized();
            Vec3 toCamera = Vec3{random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f), random.between(-1.0f, 1.0f)}.normalized();

            if (toCamera.dot(normal) < 0.05f) {
                toCamera = (toCamera - normal * (toCamera.dot(normal) - 0.05f)).normalized();
            }

            inputs.push_back(GpuVec4{random.between(-20.0f, 20.0f), random.between(-20.0f, 20.0f), random.between(-20.0f, 20.0f), 0.0f});
            inputs.push_back(gpuVec4(toCamera, 0.0f));
            inputs.push_back(gpuVec4(normal, random.between(0.0f, 2.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_REFRACTION, inputs, 6, 3);

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4* in = &inputs[i * 6];
            const GpuVec4* out = &outputs[i * 3];
            const float spread = transmissionSpread(in[0][0], in[0][1]);
            report.check("transmissionSpread", arithmeticTolerance, spread, out[0][0]);
            report.check("refractionLevel", FloatTolerance{.absolute = 1e-4f, .relative = 1e-4f},
                         refractionLevel(spread, in[0][2], in[0][3], in[1][0], static_cast<std::uint32_t>(in[1][1])), out[0][1]);
            report.check("beerAttenuation", transcendentalTolerance, beerAttenuation(vec3From(in[2]), in[1][2], in[1][3]), vec3From(out[1]));
            report.check("solidRefractionExit", FloatTolerance{.absolute = 1e-5f, .relative = 1e-5f},
                         solidRefractionExit(vec3From(in[3]), vec3From(in[4]), vec3From(in[5]), in[0][1], in[5][3]), vec3From(out[2]));
        }
    }
}

void gpuTestTransparency(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("transparency.glsl");
    checkBlend(runner, report);
    checkReactiveShare(runner, report);
    checkGlassRays(runner, report);
    checkRefraction(runner, report);
}
