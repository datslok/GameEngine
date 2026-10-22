#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/eye_adaptation.h"
#include "render/gpu/tone_mapping.h"

#include <cmath>
#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("tone_mapping_test");

    constexpr ToneMapCurve curves[] = {ToneMapCurve::Clamp, ToneMapCurve::Reinhard, ToneMapCurve::Aces, ToneMapCurve::AgX};

    const char* curveName(ToneMapCurve curve) {
        switch (curve) {
            case ToneMapCurve::Clamp: return "toneMap Clamp";
            case ToneMapCurve::Reinhard: return "toneMap Reinhard";
            case ToneMapCurve::Aces: return "toneMap ACES";
            case ToneMapCurve::AgX: return "toneMap AgX";
        }

        return "toneMap";
    }

    /*
    * Every curve over brightness from black to 1000 times white in steps of a factor of two (tone mapping works in
    * stops), plus coloured light, for an SDR screen and an HDR one with headroom, at two exposures.
    * Inputs per case: (light, exposure), (peak, curve, 0, 0).
    */
    void checkToneMap(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;
        std::vector<Vec3> lights{Vec3{0.0f, 0.0f, 0.0f}};

        for (int stop = -12; stop <= 10; ++stop) {
            const float level = std::ldexp(1.0f, stop);
            lights.push_back(Vec3{level, level, level});
            lights.push_back(Vec3{level, level * 0.5f, level * 0.1f});
        }

        for (ToneMapCurve curve : curves) {
            for (float peak : {1.0f, 4.0f}) {
                for (float exposure : {1.0f, 1.5f}) {
                    for (const Vec3& light : lights) {
                        inputs.push_back(gpuVec4(light, exposure));
                        inputs.push_back(gpuVec4(peak, static_cast<float>(curve)));
                    }
                }
            }
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TONE_MAP, inputs, 2, 1);

        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const GpuVec4* in = &inputs[i * 2];
            const ToneMapCurve curve = static_cast<ToneMapCurve>(static_cast<int>(in[1][1]));
            const ToneMappingSettings settings{.curve = curve, .exposure = in[0][3]};
            report.check(curveName(curve), transcendentalTolerance, toneMap(vec3From(in[0]), settings, in[1][0]), vec3From(outputs[i]));
        }
    }

    /*
    * Night vision over colours from saturated primaries to grey, black among them, at brightness from far below the
    * rods' threshold to far above the cones', for eyes set dark and light, with thresholds close together and far apart
    * and every blue shift.
    */
    void checkNightVision(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{61};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 512; ++i) {
            const float rods = random.between(-14.0f, -4.0f);
            const float cones = rods + random.between(0.1f, 6.0f);
            const Vec3 colour{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)};
            const float stops = random.between(rods - 4.0f, cones + 4.0f);
            const float luminance = pixelLuminance(colour);
            const Vec3 light = i % 40 == 0 || luminance <= 0.0f ? Vec3{} : colour * (std::exp2(stops) / luminance);
            inputs.push_back(gpuVec4(light, random.between(rods - 4.0f, cones + 4.0f)));
            inputs.push_back(GpuVec4{rods, cones, random.between(0.0f, 1.0f), random.between(rods - 4.0f, cones + 4.0f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_NIGHT_VISION, inputs, 2, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 2];
            const GpuVec4* out = &outputs[i * 2];
            ToneMappingSettings settings;
            settings.rodsFullEv = in[1][0];
            settings.conesFullEv = in[1][1];
            settings.nightBlueShift = in[1][2];
            const Vec3 light = vec3From(in[0]);
            report.check("nightVision", transcendentalTolerance, nightVision(light, in[0][3], settings), vec3From(out[0]));
            report.check("rodLuminance", arithmeticTolerance, rodLuminance(light), out[0][3]);
            report.check("rodShare", transcendentalTolerance, rodShare(in[1][3], in[0][3], settings), out[1][0]);
        }
    }
}

void gpuTestToneMapping(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("tone_mapping.glsl");
    checkToneMap(runner, report);
    checkNightVision(runner, report);
}
