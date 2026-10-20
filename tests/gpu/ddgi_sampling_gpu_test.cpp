#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/ddgi_sampling.h"

#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {
    const std::string libraryShader = gpuTestShader("ddgi_sampling_test");

    void checkBias(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{71};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            inputs.push_back(gpuVec4(random.direction(), random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(random.direction()));
            inputs.push_back(gpuVec4(random.between(0.25f, 4.0f), random.between(0.25f, 4.0f), random.between(0.25f, 4.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_BIAS, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const Vec3 expected = probeSelfShadowBias(vec3From(inputs[i * 3]), vec3From(inputs[i * 3 + 1]), vec3From(inputs[i * 3 + 2]), inputs[i * 3][3]);
            report.check("probeSelfShadowBias", arithmeticTolerance, expected, vec3From(outputs[i]));
        }
    }

    /*
    * Points inside and outside an uneven grid (one axis a single layer), every corner. Points are kept away from the
    * planes of probes, where floor() could round to either cell on the two sides.
    */
    void checkCell(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{72};
        const ProbeVolume volume{Vec3{-3.5f, 0.25f, 7.0f}, Vec3{1.0f, 0.75f, 1.5f}, ProbeCounts{5, 1, 4}};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            Vec3 cells{random.between(-1.5f, 5.5f), random.between(-1.5f, 1.5f), random.between(-1.5f, 4.5f)};

            for (float* axis : {&cells.x, &cells.y, &cells.z}) {
                const float fraction = *axis - std::floor(*axis);
                if (fraction < 0.01f || fraction > 0.99f) {
                    *axis += 0.5f;
                }
            }

            const Vec3 point = volume.origin + Vec3{cells.x * volume.spacing.x, cells.y * volume.spacing.y, cells.z * volume.spacing.z};
            inputs.push_back(gpuVec4(volume.origin));
            inputs.push_back(gpuVec4(volume.spacing));
            inputs.push_back(gpuVec4(static_cast<float>(volume.counts.x), static_cast<float>(volume.counts.y), static_cast<float>(volume.counts.z),
                                     static_cast<float>(i % 8)));
            inputs.push_back(gpuVec4(point));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_CELL, inputs, 4, 3);

        for (std::size_t i = 0; i < inputs.size() / 4; ++i) {
            const ProbeCell cell = probeCell(volume, vec3From(inputs[i * 4 + 3]));
            const int corner = static_cast<int>(inputs[i * 4 + 2][3]);
            const ProbeGridCoord grid = probeCellCorner(cell, volume.counts, corner);
            const GpuVec4* out = &outputs[i * 3];

            report.check("probeCell base", exactTolerance,
                         Vec3{static_cast<float>(cell.base.x), static_cast<float>(cell.base.y), static_cast<float>(cell.base.z)}, vec3From(out[0]));
            report.check("probeCell alpha", arithmeticTolerance, cell.alpha, vec3From(out[1]));
            report.check("probeCellCorner", exactTolerance, Vec3{static_cast<float>(grid.x), static_cast<float>(grid.y), static_cast<float>(grid.z)},
                         vec3From(out[2]));
            report.check("probeTrilinearWeight", arithmeticTolerance, probeTrilinearWeight(cell.alpha, corner), out[2][3]);
        }
    }

    /*
    * The variance is the mean square less the mean's square, which cancels to a small difference of large numbers when
    * the spread is small beside the mean: a rounding of the mean square (or a fused multiply-add on the GPU) moves it by
    * a few units in the last place of the mean square, a share mean square / variance larger of the variance, and the
    * cube triples that.
    */
    FloatTolerance visibilityTolerance(const Vec2& moments) {
        const float variance = std::abs(moments.y - moments.x * moments.x);
        FloatTolerance tolerance = arithmeticTolerance;
        tolerance.relative += 3.0f * 4.0f * std::numeric_limits<float>::epsilon() * moments.y / std::max(variance, 1e-12f);
        return tolerance;
    }

    /*
    * Probes all round a surface, at distances either side of their mean (kept clear of it, where the visibility jumps
    * between 1 and the bound), and weights either side of the crush threshold.
    */
    void checkCornerWeights(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{73};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const float mean = random.between(0.1f, 3.0f);
            const float spread = random.between(0.0f, 0.5f);
            float distance = mean + random.between(-1.0f, 2.0f);

            if (std::abs(distance - mean) < 1e-3f) {
                distance += 0.01f;
            }

            const Vec3 toProbe = i % 9 == 0 ? Vec3{0.0f, 0.0f, 0.0f} : random.direction() * random.between(0.1f, 3.0f);
            inputs.push_back(gpuVec4(random.direction()));
            inputs.push_back(gpuVec4(toProbe));
            inputs.push_back(gpuVec4(std::max(distance, 0.0f), mean, mean * mean + spread * spread, random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 0.6f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_CORNER_WEIGHTS, inputs, 4, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const Vec3 normal = vec3From(inputs[i * 4]);
            const Vec3 toProbe = vec3From(inputs[i * 4 + 1]);
            const GpuVec4& in = inputs[i * 4 + 2];
            const Vec2 moments{in[1], in[2]};
            const GpuVec4& out = outputs[i];

            report.check("probeBackFaceWeight", arithmeticTolerance, probeBackFaceWeight(normal, toProbe), out[0]);
            report.check("probeVisibility", visibilityTolerance(moments), probeVisibility(in[0], moments), out[1]);
            report.check("crushProbeWeight", arithmeticTolerance, crushProbeWeight(inputs[i * 4 + 3][0]), out[2]);
            report.check("probeCornerWeight", visibilityTolerance(moments), probeCornerWeight(normal, toProbe, in[0], moments, in[3]), out[3]);
        }
    }

    // Points in, across and beyond a cascade's fade, round random centres.
    void checkCascadeWeight(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{74};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 400; ++i) {
            const Vec3 centre{random.between(-20.0f, 20.0f), random.between(-5.0f, 5.0f), random.between(-20.0f, 20.0f)};
            const Vec3 reach{random.between(1.0f, 10.0f), random.between(1.0f, 5.0f), random.between(1.0f, 10.0f)};
            const Vec3 fade{random.between(0.2f, 1.0f), random.between(0.2f, 1.0f), random.between(0.2f, 1.0f)};
            inputs.push_back(gpuVec4(centre));
            inputs.push_back(gpuVec4(reach));
            inputs.push_back(gpuVec4(fade));
            inputs.push_back(gpuVec4(centre + Vec3{random.between(-12.0f, 12.0f), random.between(-6.0f, 6.0f), random.between(-12.0f, 12.0f)}));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_CASCADE_WEIGHT, inputs, 4, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const float expected = probeCascadeWeight(vec3From(inputs[i * 4]), vec3From(inputs[i * 4 + 1]), vec3From(inputs[i * 4 + 2]), vec3From(inputs[i * 4 + 3]));
            report.check("probeCascadeWeight", arithmeticTolerance, expected, outputs[i][0]);
        }
    }

    void checkBlend(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{75};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            for (int sample = 0; sample < 3; ++sample) {
                const Vec3 irradiance{random.between(0.0f, 20.0f), random.between(0.0f, 2.0f), random.between(0.0f, 0.05f)};
                inputs.push_back(gpuVec4(irradiance, random.between(1e-4f, 1.0f)));
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_PROBE_BLEND, inputs, 3, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            ProbeBlend blend;

            for (int sample = 0; sample < 3; ++sample) {
                const GpuVec4& in = inputs[i * 3 + static_cast<std::size_t>(sample)];
                addProbeSample(blend, vec3From(in), in[3]);
            }

            report.check("addProbeSample, finishProbeBlend", arithmeticTolerance, finishProbeBlend(blend), vec3From(outputs[i]));
        }
    }

    // Lights all round a surface, some out of range or behind it, spots pointing anywhere.
    void checkHitLights(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{76};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 500; ++i) {
            const float outer = random.between(0.2f, 1.2f);
            const float inner = outer * random.between(0.2f, 0.9f);
            const Vec3 offset = random.direction() * random.between(0.05f, 12.0f);
            // Half the spots aimed at the surface, the rest anywhere.
            const Vec3 beam = i % 2 == 0 ? (offset * -1.0f + random.direction() * 0.3f).normalized() : random.direction();
            inputs.push_back(gpuVec4(offset, random.between(1.0f, 10.0f)));
            inputs.push_back(gpuVec4(random.between(0.0f, 50.0f), random.between(0.0f, 5.0f), random.between(0.0f, 1.0f), random.between(0.05f, 1.0f)));
            inputs.push_back(gpuVec4(beam, std::cos(outer)));
            inputs.push_back(gpuVec4(random.direction(), std::cos(inner)));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_HIT_LIGHTS, inputs, 4, 2);

        for (std::size_t i = 0; i < inputs.size() / 4; ++i) {
            const GpuVec4* in = &inputs[i * 4];
            const Vec3 offset = vec3From(in[0]);
            const Vec3 radiance = vec3From(in[1]);
            const Vec3 normal = vec3From(in[3]);
            const Vec3 point = pointLightAtHit(offset, in[0][3], radiance, in[1][3], normal);

            // smoothstep's cubic near the cone's edges amplifies rounding in the cosine.
            report.check("pointLightAtHit", arithmeticTolerance, point, vec3From(outputs[i * 2]));
            report.check("hitLightStrength", arithmeticTolerance, hitLightStrength(point), outputs[i * 2][3]);
            report.check("spotLightAtHit", transcendentalTolerance,
                         spotLightAtHit(offset, in[0][3], radiance, in[1][3], vec3From(in[2]), in[2][3], in[3][3], normal), vec3From(outputs[i * 2 + 1]));
        }
    }

    // Eight candidates at a time, with ties and lights giving nothing, kept by every capacity.
    void checkBrightestLights(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{77};
        std::vector<GpuVec4> inputs;
        const float steps[] = {0.0f, 0.5f, 1.0f, 2.0f};

        for (int i = 0; i < 300; ++i) {
            float strengths[8];

            for (float& strength : strengths) {
                // Mostly a few repeated values, so ties are common; some from a continuous range.
                strength = i % 3 == 0 ? random.between(0.0f, 3.0f) : steps[static_cast<int>(random.between(0.0f, 3.999f))];
            }

            inputs.push_back(gpuVec4(strengths[0], strengths[1], strengths[2], strengths[3]));
            inputs.push_back(gpuVec4(strengths[4], strengths[5], strengths[6], strengths[7]));
            inputs.push_back(gpuVec4(static_cast<float>(i % (maxLightsPerHit + 1))));
        }

        const std::vector<GpuVec4> outputs = runner.run(libraryShader, GPU_TEST_BRIGHTEST_LIGHTS, inputs, 3, 3);

        for (std::size_t i = 0; i < inputs.size() / 3; ++i) {
            BrightestLights kept;

            for (int light = 0; light < 8; ++light) {
                keepBrightestLight(kept, static_cast<int>(inputs[i * 3 + 2][0]), light, inputs[i * 3 + static_cast<std::size_t>(light / 4)][static_cast<std::size_t>(light % 4)]);
            }

            float expected[8];

            for (int slot = 0; slot < 8; ++slot) {
                expected[slot] = slot < kept.count ? static_cast<float>(kept.lights[slot]) : -1.0f;
            }

            const GpuVec4* out = &outputs[i * 3];
            report.check("keepBrightestLight count", exactTolerance, static_cast<float>(kept.count), out[2][0]);
            report.check("keepBrightestLight", exactTolerance, Vec3{expected[0], expected[1], expected[2]}, vec3From(out[0]));
            report.check("keepBrightestLight", exactTolerance, Vec3{expected[3], expected[4], expected[5]}, Vec3{out[0][3], out[1][0], out[1][1]});
            report.check("keepBrightestLight", exactTolerance, Vec3{expected[6], expected[7], 0.0f}, Vec3{out[1][2], out[1][3], 0.0f});
        }
    }
}

void gpuTestDdgiSampling(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("ddgi_sampling.glsl");
    checkBias(runner, report);
    checkCell(runner, report);
    checkCornerWeights(runner, report);
    checkCascadeWeight(runner, report);
    checkBlend(runner, report);
    checkHitLights(runner, report);
    checkBrightestLights(runner, report);
}
