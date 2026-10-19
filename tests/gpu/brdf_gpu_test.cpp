#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/brdf.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>

namespace {
    const std::string shader = gpuTestShader("brdf_test");

    // Roughness from the smallest the shader allows to 1, where GGX goes from a near-mirror's spike to a broad lobe.
    std::vector<float> roughnessSteps() {
        std::vector<float> steps;

        for (int i = 0; i <= 10; ++i) {
            steps.push_back(smallestRoughness + (1.0f - smallestRoughness) * static_cast<float>(i) / 10.0f);
        }

        return steps;
    }

    /*
    * GGX over the whole hemisphere of halfway angles and every roughness. Near a mirror (small alpha, nDotH near 1) the
    * denominator nDotH^2 (alpha^2 - 1) + 1 subtracts nearly equal numbers, so a fused multiply-add there changes the
    * last bits of a small result by more than usual: the case most likely to need a looser tolerance.
    */
    void checkGgx(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (float roughness : roughnessSteps()) {
            for (int i = 0; i <= 40; ++i) {
                const float nDotH = static_cast<float>(i) / 40.0f;
                inputs.push_back(gpuVec4(nDotH, roughness * roughness));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_GGX_DISTRIBUTION, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("ggxDistribution", arithmeticTolerance, ggxDistribution(inputs[i][0], inputs[i][1]), outputs[i][0]);
        }
    }

    // Smith's visibility for every pair of cosines from the shader's smallest view cosine up, at every roughness.
    void checkSmith(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;
        const float smallest = static_cast<float>(BRDF_SMALLEST_VIEW_COSINE);

        for (float roughness : roughnessSteps()) {
            for (float nDotV : {smallest, 0.01f, 0.1f, 0.3f, 0.5f, 0.7f, 0.9f, 1.0f}) {
                for (float nDotL : {smallest, 0.01f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f}) {
                    inputs.push_back(gpuVec4(nDotV, nDotL, roughness * roughness));
                }
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SMITH_VISIBILITY, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("smithVisibility", arithmeticTolerance, smithVisibility(inputs[i][0], inputs[i][1], inputs[i][2]), outputs[i][0]);
        }
    }

    // Schlick's Fresnel for a dielectric, coloured metals and white, at cosines past both ends (clamped) as well.
    void checkFresnel(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (const Vec3& f0 : {Vec3{0.04f, 0.04f, 0.04f}, Vec3{1.0f, 0.71f, 0.29f}, Vec3{0.95f, 0.64f, 0.54f}, Vec3{1.0f, 1.0f, 1.0f}}) {
            for (int i = -2; i <= 22; ++i) {
                inputs.push_back(gpuVec4(f0, static_cast<float>(i) / 20.0f));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_SCHLICK_FRESNEL, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("schlickFresnel", transcendentalTolerance, schlickFresnel(vec3From(inputs[i]), inputs[i][3]), vec3From(outputs[i]));
        }
    }

    /*
    * Random surfaces seen from random directions, as the shader meets them. The table entry is looked up on the CPU and
    * handed to the shader, which reads it from a texture in the real pass (the texture filtering itself is left to the
    * golden images).
    * Inputs per case: (base colour, metallic), (normal, roughness), (toCamera, 0), (table entry, 0, 0), (radiance, 0), (toLight, 0).
    */
    struct ShadingCase {
        SurfaceProperties surface;
        Vec3 normal;
        Vec3 toCamera;
        Vec3 radiance;
        Vec3 toLight;
    };

    std::vector<ShadingCase> shadingCases() {
        CaseRandom random{2026};
        std::vector<ShadingCase> cases;

        for (int i = 0; i < 400; ++i) {
            ShadingCase shading;
            shading.surface.baseColour = Vec3{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)};
            shading.surface.metallic = i % 4 == 0 ? 0.0f : (i % 4 == 1 ? 1.0f : random.between(0.0f, 1.0f));
            shading.surface.roughness = random.between(smallestRoughness, 1.0f);
            shading.normal = random.direction();
            shading.toCamera = random.direction();
            shading.radiance = Vec3{random.between(0.0f, 20.0f), random.between(0.0f, 20.0f), random.between(0.0f, 20.0f)};
            shading.toLight = random.direction();

            // Most cases see the lit side, as the screen mostly does; the rest check the cut-offs behind the surface.
            if (i % 5 != 0) {
                if (shading.toCamera.dot(shading.normal) < 0.0f) {
                    shading.toCamera = shading.toCamera * -1.0f;
                }

                if (shading.toLight.dot(shading.normal) < 0.0f) {
                    shading.toLight = shading.toLight * -1.0f;
                }
            }

            cases.push_back(shading);
        }

        return cases;
    }

    std::vector<GpuVec4> shadingInputs(const std::vector<ShadingCase>& cases, const BrdfTable& table) {
        std::vector<GpuVec4> inputs;

        for (const ShadingCase& shading : cases) {
            const float nDotV = std::clamp(shading.normal.dot(shading.toCamera), static_cast<float>(BRDF_SMALLEST_VIEW_COSINE), 1.0f);
            const Vec2 entry = sampleBrdfTable(table, nDotV, shading.surface.roughness);
            inputs.push_back(gpuVec4(shading.surface.baseColour, shading.surface.metallic));
            inputs.push_back(gpuVec4(shading.normal, shading.surface.roughness));
            inputs.push_back(gpuVec4(shading.toCamera));
            inputs.push_back(gpuVec4(entry.x, entry.y));
            inputs.push_back(gpuVec4(shading.radiance));
            inputs.push_back(gpuVec4(shading.toLight));
        }

        return inputs;
    }

    // Outputs per case: (f0, nDotV), (energy compensation, alpha), (specular albedo, 0), (diffuse colour, 0).
    void checkPrepareShading(GpuFunctionRunner& runner, GpuCheckReport& report, const std::vector<ShadingCase>& cases, const BrdfTable& table) {
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_PREPARE_SHADING, shadingInputs(cases, table), 6, 4);

        for (std::size_t i = 0; i < cases.size(); ++i) {
            const ShadingPoint expected = prepareShading(cases[i].surface, cases[i].normal, cases[i].toCamera, table);
            const GpuVec4* actual = &outputs[i * 4];
            report.check("prepareShading f0", arithmeticTolerance, expected.f0, vec3From(actual[0]));
            report.check("prepareShading nDotV", arithmeticTolerance, expected.nDotV, actual[0][3]);
            report.check("prepareShading energyCompensation", arithmeticTolerance, expected.energyCompensation, vec3From(actual[1]));
            report.check("prepareShading alpha", arithmeticTolerance, expected.alpha, actual[1][3]);
            report.check("prepareShading specularAlbedo", arithmeticTolerance, expected.specularAlbedo, vec3From(actual[2]));
            report.check("prepareShading diffuse", arithmeticTolerance, expected.diffuse, vec3From(actual[3]));
        }
    }

    /*
    * A sharp highlight seen near its peak is ill-conditioned: its brightness changes enormously with the angle. GGX is
    * alpha^2 / (pi d^2) with d = 1 - nDotH^2 (1 - alpha^2), which near the peak is a small difference of numbers close
    * to 1. Each side works out the halfway vector, and so nDotH, its own way (the GPU's length and division may differ
    * by a float step or two), and a change of delta in nDotH changes d by about 2 delta, so D by about
    * 4 delta / d relative. At alpha = 0.03, 1e-4 from the peak, d is about 1e-3 and one float step of nDotH (6e-8)
    * already moves the result by 2.4e-4: the GPU found exactly that. So the allowance grows with 1 / d (the problem's
    * condition number), for a nDotH uncertainty of 4 epsilon (5e-7, eight float steps just below 1); away from sharp
    * peaks it is the usual one.
    * Diffuse light has no such peak, which is why addLight's diffuse part keeps the plain tolerance.
    */
    FloatTolerance specularTolerance(const ShadingPoint& point, const Vec3& toLight) {
        const Vec3 sum = toLight + point.toCamera;
        const float sumLength = sum.length();

        if (!(sumLength > 0.0f)) {
            return transcendentalTolerance;
        }

        const float nDotH = std::max(point.normal.dot(sum * (1.0f / sumLength)), 0.0f);
        const float denominator = 1.0f - nDotH * nDotH * (1.0f - point.alpha * point.alpha);
        const float nDotHUncertainty = 4.0f * std::numeric_limits<float>::epsilon();
        FloatTolerance tolerance = transcendentalTolerance;
        tolerance.relative += 4.0f * nDotHUncertainty / std::max(denominator, point.alpha * point.alpha);
        return tolerance;
    }

    // Outputs per case: (diffuse, 0), (specular, 0), (ambient reflected under the radiance as ambient light, 0).
    void checkAddLight(GpuFunctionRunner& runner, GpuCheckReport& report, const std::vector<ShadingCase>& cases, const BrdfTable& table) {
        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_ADD_LIGHT, shadingInputs(cases, table), 6, 3);

        for (std::size_t i = 0; i < cases.size(); ++i) {
            const ShadingPoint point = prepareShading(cases[i].surface, cases[i].normal, cases[i].toCamera, table);
            const LightResponse expected = shadeLight(point, cases[i].toLight, cases[i].radiance);
            const GpuVec4* actual = &outputs[i * 3];
            report.check("addLight diffuse", arithmeticTolerance, expected.diffuse, vec3From(actual[0]));
            report.check("addLight specular", specularTolerance(point, cases[i].toLight), expected.specular, vec3From(actual[1]));
            report.check("shadeAmbient", arithmeticTolerance, shadeAmbient(point, cases[i].radiance), vec3From(actual[2]));
        }
    }
}

void gpuTestBrdf(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("brdf.glsl");
    checkGgx(runner, report);
    checkSmith(runner, report);
    checkFresnel(runner, report);

    const BrdfTable table = buildBrdfTable();
    const std::vector<ShadingCase> cases = shadingCases();
    checkPrepareShading(runner, report, cases, table);
    checkAddLight(runner, report, cases, table);
}
