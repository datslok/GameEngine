#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/taa.h"

#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("taa_test");

    // Screens of several sizes, points on and off them, motions small and large. Inputs per case: (ndc, pixel), (size, motion).
    void checkPixels(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{71};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const Vec2 size{random.between(64.0f, 3840.0f), random.between(64.0f, 2160.0f)};
            inputs.push_back(GpuVec4{random.between(-1.2f, 1.2f), random.between(-1.2f, 1.2f), random.between(0.0f, size.x), random.between(0.0f, size.y)});
            inputs.push_back(GpuVec4{size.x, size.y, random.between(-0.5f, 0.5f), random.between(-0.5f, 0.5f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TAA_PIXELS, inputs, 2, 2);
        const FloatTolerance pixelTolerance{.absolute = 1e-3f, .relative = 1e-5f};

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 2];
            const GpuVec4* out = &outputs[i * 2];
            const Vec2 size{in[1][0], in[1][1]};
            const Vec2 pixel = ndcToPixel(Vec2{in[0][0], in[0][1]}, size);
            const Vec2 ndc = pixelToNdc(Vec2{in[0][2], in[0][3]}, size);
            const Vec2 previous = previousPixelFromMotion(Vec2{in[0][2], in[0][3]}, Vec2{in[1][2], in[1][3]}, size);
            report.check("ndcToPixel", pixelTolerance, Vec3{pixel.x, pixel.y, 0.0f}, Vec3{out[0][0], out[0][1], 0.0f});
            report.check("pixelToNdc", arithmeticTolerance, Vec3{ndc.x, ndc.y, 0.0f}, Vec3{out[0][2], out[0][3], 0.0f});
            report.check("previousPixelFromMotion", pixelTolerance, Vec3{previous.x, previous.y, 0.0f}, Vec3{out[1][0], out[1][1], 0.0f});
        }
    }

    /*
    * Colours from black to far brighter than white, boxes thin and wide (and flat), weights from none to all.
    * Inputs per case: (history, weight), (centre), (extent), (current).
    */
    void checkColours(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{73};
        std::vector<GpuVec4> inputs;
        const auto colour = [&](float scale) { return Vec3{random.between(0.0f, scale), random.between(0.0f, scale), random.between(0.0f, scale)}; };

        for (int i = 0; i < 256; ++i) {
            const float scale = i % 4 == 0 ? 100.0f : 2.0f;
            inputs.push_back(gpuVec4(colour(scale), i % 16 == 0 ? 1.0f : random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, scale), random.between(-0.5f, 0.5f), random.between(-0.5f, 0.5f)}));
            inputs.push_back(gpuVec4(i % 10 == 0 ? Vec3{} : colour(0.5f)));
            inputs.push_back(gpuVec4(colour(scale)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TAA_COLOURS, inputs, 4, 4);

        for (std::size_t i = 0; i < outputs.size() / 4; ++i) {
            const GpuVec4* in = &inputs[i * 4];
            const GpuVec4* out = &outputs[i * 4];
            const Vec3 history = vec3From(in[0]);
            report.check("toYCoCg", arithmeticTolerance, toYCoCg(history), vec3From(out[0]));
            report.check("fromYCoCg", arithmeticTolerance, fromYCoCg(history), vec3From(out[1]));
            report.check("clipToBox", arithmeticTolerance, clipToBox(history, vec3From(in[1]), vec3From(in[2])), vec3From(out[2]));
            report.check("taaBlend", arithmeticTolerance, taaBlend(vec3From(in[3]), history, in[0][3]), vec3From(out[3]));
        }
    }

    // Catmull-Rom's weights over a texel, the Halton points and the jitter over several cycles. Inputs: (t, frame, 0, 0).
    void checkWeights(GpuFunctionRunner& runner, GpuCheckReport& report) {
        std::vector<GpuVec4> inputs;

        for (int i = 0; i <= 100; ++i) {
            inputs.push_back(GpuVec4{static_cast<float>(i) / 100.0f, static_cast<float>(i), 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TAA_WEIGHTS, inputs, 1, 3);

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4& in = inputs[i];
            const GpuVec4* out = &outputs[i * 3];
            const std::uint32_t frame = static_cast<std::uint32_t>(in[1]);
            const Vec2 jitter = taaJitter(frame);
            const Vec4 weights = catmullRomWeights(in[0]);
            report.check("catmullRomWeights", arithmeticTolerance, Vec3{weights.x, weights.y, weights.z}, vec3From(out[0]));
            report.check("catmullRomWeights", arithmeticTolerance, weights.w, out[0][3]);
            report.check("halton", arithmeticTolerance, Vec3{halton(frame, 2), halton(frame, 3), 0.0f}, Vec3{out[1][0], out[1][1], 0.0f});
            report.check("taaJitter", arithmeticTolerance, Vec3{jitter.x, jitter.y, 0.0f}, Vec3{out[1][2], out[1][3], 0.0f});

            // Cycles of FSR's lengths, 8 to 72 phases.
            const std::uint32_t phases = 8 + frame % 65;
            const Vec2 cycled = taaJitter(frame, phases);
            report.check("taaJitter phases", arithmeticTolerance, Vec3{cycled.x, cycled.y, 0.0f}, Vec3{out[2][0], out[2][1], 0.0f});
        }
    }

    /*
    * Reprojections between cameras that moved and turned, pixels all over the screen at depths from the near plane to
    * the far one; and objects' motion from clip positions, some behind the camera. Inputs per case: the reprojection's
    * four columns, (pixel, depth, 0), (size, 0, 0), (current clip), (previous clip).
    */
    void checkReprojection(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{79};
        std::vector<GpuVec4> inputs;
        std::vector<Mat4> reprojections;

        for (int i = 0; i < 256; ++i) {
            const Vec2 size{1280.0f, 720.0f};
            const Mat4 lens = Mat4::perspective(random.between(0.6f, 1.6f), size.x / size.y, 0.1f, 500.0f);
            const Mat4 current = lens * Mat4::rotationY(random.between(-3.0f, 3.0f));
            const Mat4 previous = lens * Mat4::rotationY(random.between(-3.0f, 3.0f)) * Mat4::translation(random.between(-1.0f, 1.0f), 0.0f, random.between(-1.0f, 1.0f));
            const Mat4 reprojection = taaReprojection(current, previous);
            reprojections.push_back(reprojection);

            for (int column = 0; column < 4; ++column) {
                inputs.push_back(GpuVec4{reprojection.values[0][column], reprojection.values[1][column], reprojection.values[2][column],
                                         reprojection.values[3][column]});
            }

            inputs.push_back(GpuVec4{random.between(0.0f, size.x), random.between(0.0f, size.y), random.between(0.0f, 1.0f), 0.0f});
            inputs.push_back(GpuVec4{size.x, size.y, 0.0f, 0.0f});
            inputs.push_back(GpuVec4{random.between(-5.0f, 5.0f), random.between(-5.0f, 5.0f), random.between(0.0f, 5.0f), random.between(0.1f, 5.0f)});
            inputs.push_back(GpuVec4{random.between(-5.0f, 5.0f), random.between(-5.0f, 5.0f), random.between(0.0f, 5.0f), random.between(-1.0f, 5.0f)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TAA_REPROJECTION, inputs, 8, 3);
        const FloatTolerance pixelTolerance{.absolute = 1e-2f, .relative = 1e-4f};
        const FloatTolerance ndcTolerance{.absolute = 1e-5f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4* in = &inputs[i * 8];
            const GpuVec4* out = &outputs[i * 3];
            const Reprojected found = reprojectPixel(reprojections[i], Vec2{in[4][0], in[4][1]}, in[4][2], Vec2{in[5][0], in[5][1]});
            const Vec2 motion = objectMotion(Vec4{in[6][0], in[6][1], in[6][2], in[6][3]}, Vec4{in[7][0], in[7][1], in[7][2], in[7][3]});
            report.check("reprojectPixel valid", exactTolerance, found.valid ? 1.0f : 0.0f, out[0][2]);

            if (found.valid && out[0][2] > 0.5f) {
                report.check("reprojectPixel", pixelTolerance, Vec3{found.pixel.x, found.pixel.y, 0.0f}, Vec3{out[0][0], out[0][1], 0.0f});
            }

            report.check("objectMotion", transcendentalTolerance, Vec3{motion.x, motion.y, 0.0f}, Vec3{out[1][0], out[1][1], 0.0f});

            if (found.valid && out[0][2] > 0.5f) {
                const Vec2 camera = cameraMotion(reprojections[i], Vec2{in[4][0], in[4][1]}, in[4][2], Vec2{in[5][0], in[5][1]});
                report.check("cameraMotion", ndcTolerance, Vec3{camera.x, camera.y, 0.0f}, Vec3{out[2][0], out[2][1], 0.0f});
            }
        }
    }
}

namespace {
    /*
    * Upscaling: render pixels anywhere on a 4K screen, jitters within half a pixel, scales from a third to one,
    * squared distances from dead on to far past the 3x3 texels' reach. Inputs per case: (render pixel, jitter),
    * (output per render, squared distance, current weight), (reactive share).
    */
    void checkUpscaling(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{83};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const float scale = random.between(1.0f, 3.0f);
            inputs.push_back(GpuVec4{random.between(0.0f, 3840.0f), random.between(0.0f, 2160.0f), random.between(-0.5f, 0.5f), random.between(-0.5f, 0.5f)});
            inputs.push_back(GpuVec4{scale, scale * random.between(0.95f, 1.05f), i % 16 == 0 ? 0.0f : random.between(0.0f, 20.0f), random.between(0.01f, 1.0f)});
            inputs.push_back(GpuVec4{i % 4 == 0 ? 0.0f : random.between(0.0f, 1.0f), 0.0f, 0.0f, 0.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_TAA_UPSCALING, inputs, 3, 2);
        const FloatTolerance pixelTolerance{.absolute = 1e-3f, .relative = 1e-5f};

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 3];
            const GpuVec4* out = &outputs[i * 2];
            const Vec2 scales{in[1][0], in[1][1]};
            const Vec2 sample = sampleOutputPixel(Vec2{in[0][0], in[0][1]}, Vec2{in[0][2], in[0][3]}, scales);
            const Vec2 onRender = renderPixelOf(Vec2{in[0][0], in[0][1]}, scales);
            const float weight = taaSampleWeight(in[1][2]);
            report.check("sampleOutputPixel", pixelTolerance, Vec3{sample.x, sample.y, 0.0f}, Vec3{out[0][0], out[0][1], 0.0f});
            report.check("renderPixelOf", pixelTolerance, Vec3{onRender.x, onRender.y, 0.0f}, Vec3{out[0][2], out[0][3], 0.0f});
            report.check("taaSampleWeight", transcendentalTolerance, weight, out[1][0]);
            report.check("taaCurrentShare", transcendentalTolerance, taaCurrentShare(in[1][3], weight), out[1][1]);
            report.check("taaReactiveShare", transcendentalTolerance, taaReactiveShare(taaCurrentShare(in[1][3], weight), in[2][0]), out[1][2]);
        }
    }
}

void gpuTestTaa(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("taa.glsl");
    checkPixels(runner, report);
    checkColours(runner, report);
    checkWeights(runner, report);
    checkReprojection(runner, report);
    checkUpscaling(runner, report);
}
