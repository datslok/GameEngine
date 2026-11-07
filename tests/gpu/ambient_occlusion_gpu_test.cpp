#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/ambient_occlusion.h"

#include <bit>
#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("ambient_occlusion_test");

    /*
    * Normals and ways to the camera every way, slices at every angle, horizons from straight up to below the surface,
    * neighbours in line and off it, every roughness, visibility and albedo. Inputs per case: (normal, radius),
    * (toCamera, falloff), (direction, distance), (offset, noise), (centre, roughness), (before, nDotV), (before next,
    * visibility), (after, footprint), (after next, view distance), (albedo, horizon cosine 0), (horizon cosine 1, screen
    * radius, step, count).
    */
    void checkOcclusion(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{151};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const Vec3 centre = random.direction() * random.between(0.5f, 50.0f);
            const Vec3 along = random.direction() * random.between(0.001f, 0.2f);
            const bool inLine = i % 2 == 0;
            const float radius = random.between(0.05f, 3.0f);
            inputs.push_back(gpuVec4(random.direction(), radius));
            inputs.push_back(gpuVec4(random.direction(), random.between(0.05f, 1.0f)));
            inputs.push_back(gpuVec4(random.direction(), random.between(0.0f, radius * 1.2f)));
            inputs.push_back(gpuVec4(random.direction() * random.between(0.01f, 2.0f), random.between(0.0f, 0.999f)));
            inputs.push_back(gpuVec4(centre, random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(centre - along, random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(inLine ? centre - along * 2.0f : centre - along * 2.0f + random.direction() * 0.05f, random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(centre + along * random.between(0.5f, 3.0f), random.between(0.0005f, 0.01f)));
            inputs.push_back(gpuVec4(centre + random.direction() * 0.3f, random.between(0.2f, 80.0f)));
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)}, random.between(-1.0f, 1.0f)));
            inputs.push_back(GpuVec4{random.between(-1.0f, 1.0f), random.between(0.0f, 300.0f), static_cast<float>(i % 6), 6.0f});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_AMBIENT_OCCLUSION, inputs, 11, 8);
        // acos near its ends and the cross products near parallel steps can lose digits.
        const FloatTolerance angleTolerance{.absolute = 1e-4f, .relative = 1e-3f};

        for (std::size_t i = 0; i < outputs.size() / 8; ++i) {
            const GpuVec4* in = &inputs[i * 11];
            const GpuVec4* out = &outputs[i * 8];
            const Vec3 normal = vec3From(in[0]);
            const Vec3 toCamera = vec3From(in[1]);
            const AoSlice slice = aoSlice(normal, toCamera, vec3From(in[2]));

            report.check("occluderWeight", arithmeticTolerance, occluderWeight(in[2][3], in[0][3], in[1][3]), out[0][0]);
            report.check("lowHorizonCosine", angleTolerance, Vec3{lowHorizonCosine(slice, 0), lowHorizonCosine(slice, 1), 0.0f},
                         Vec3{out[0][1], out[0][2], 0.0f});
            report.check("horizonCosine", arithmeticTolerance, horizonCosine(vec3From(in[3]), toCamera), out[0][3]);
            report.check("aoSlice", angleTolerance, slice.axis, vec3From(out[1]));
            report.check("aoSlice", angleTolerance, Vec3{slice.normalAngle, slice.projectedLength, 0.0f}, Vec3{out[1][3], out[2][0], 0.0f});
            report.check("sliceVisibility", angleTolerance, sliceVisibility(slice, in[9][3], in[10][0]), out[2][1]);
            report.check("aoScreenRadius", arithmeticTolerance, aoScreenRadius(in[0][3], in[7][3], in[8][3]), out[2][2]);
            report.check("aoSampleDistance", arithmeticTolerance,
                         aoSampleDistance(static_cast<int>(in[10][2]), static_cast<int>(in[10][3]), in[3][3], in[10][1]), out[2][3]);

            const Vec3 centre = vec3From(in[4]);
            const Vec3 step = surfaceStep(centre, vec3From(in[5]), vec3From(in[6]), vec3From(in[7]), vec3From(in[8]));
            report.check("surfaceStep", arithmeticTolerance, step, vec3From(out[3]));
            report.check("specularOcclusion", transcendentalTolerance, specularOcclusion(in[5][3], in[6][3], in[4][3]), out[3][3]);
            report.check("depthNormal", angleTolerance, depthNormal(centre, step, vec3From(in[3])), vec3From(out[4]));
            report.check("aoBlurWeight", arithmeticTolerance, aoBlurWeight(in[8][3], in[8][3] * 1.04f), out[4][3]);
            report.check("multiBounceOcclusion", arithmeticTolerance, multiBounceOcclusion(in[6][3], vec3From(in[9])), vec3From(out[5]));
            report.check("surfaceVisibility", arithmeticTolerance, surfaceVisibility(in[6][3], in[4][3], in[1][3]), out[5][3]);
            report.check("sectorShare", angleTolerance, Vec3{sectorShare(slice, in[9][3], 0), sectorShare(slice, in[10][0], 1), 0.0f},
                         Vec3{out[6][0], out[6][1], 0.0f});
            report.check("sectorMask", arithmeticTolerance, static_cast<float>(std::popcount(sectorMask(in[5][3], in[6][3]))), out[6][2]);
            report.check("gatheredSampleLight", arithmeticTolerance, gatheredSampleLight(vec3From(in[9]), in[6][3]), vec3From(out[7]));
        }
    }

    /*
    * The temporal accumulation's functions: reprojections near the identity (a camera's small step and turn, some
    * the identity itself), pixels anywhere on screen and a little off it, ages either side of the cap, depths within and
    * past the share. Inputs per case: the reprojection's four columns, (pixel, depth, view depth), (size, previous age,
    * history frames), (current), (history light), (expected depth, stored depth, share, tap).
    */
    void checkHistory(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{152};
        std::vector<GpuVec4> inputs;
        std::vector<Mat4> reprojections;

        for (int i = 0; i < 256; ++i) {
            Mat4 reprojection = Mat4::identity();

            if (i % 4 != 0) {
                for (int row = 0; row < 4; ++row) {
                    for (int column = 0; column < 4; ++column) {
                        reprojection.values[row][column] += random.between(-0.02f, 0.02f);
                    }
                }
            }

            reprojections.push_back(reprojection);

            for (int column = 0; column < 4; ++column) {
                inputs.push_back(GpuVec4{reprojection.values[0][column], reprojection.values[1][column], reprojection.values[2][column], reprojection.values[3][column]});
            }

            const float width = random.between(64.0f, 1920.0f);
            const float height = random.between(64.0f, 1080.0f);
            const float expected = random.between(0.2f, 200.0f);
            inputs.push_back(GpuVec4{random.between(-2.0f, width + 2.0f), random.between(-2.0f, height + 2.0f), random.between(0.0001f, 1.0f), random.between(0.2f, 200.0f)});
            inputs.push_back(GpuVec4{width, height, std::floor(random.between(0.0f, 40.0f)) + (i % 3 == 0 ? random.between(0.0f, 1.0f) : 0.0f),
                                     std::floor(random.between(1.0f, static_cast<float>(AO_MAX_HISTORY_FRAMES)))});
            inputs.push_back(GpuVec4{random.between(0.0f, 8.0f), random.between(0.0f, 8.0f), random.between(0.0f, 8.0f), random.between(0.0f, 1.0f)});
            inputs.push_back(GpuVec4{random.between(0.0f, 8.0f), random.between(0.0f, 8.0f), random.between(0.0f, 8.0f), random.between(0.0f, 1.0f)});
            inputs.push_back(GpuVec4{expected, i % 5 == 0 ? 0.0f : expected * random.between(0.9f, 1.1f), random.between(0.01f, 0.1f), static_cast<float>(i % 4)});
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_AO_HISTORY, inputs, 9, 3);

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4* in = &inputs[i * 9];
            const GpuVec4* out = &outputs[i * 3];
            const Vec2 pixel{in[4][0], in[4][1]};
            const Vec2 size{in[5][0], in[5][1]};
            const AoPreviousPlace place = aoPreviousPlace(reprojections[i], pixel, in[4][2], in[4][3], size);
            const float age = aoHistoryAge(in[5][2], static_cast<int>(in[5][3]));
            const Vec2 fraction{pixel.x - std::floor(pixel.x), pixel.y - std::floor(pixel.y)};

            // A place right on the screen's edge may fall either side of it by rounding: only compare agreed ones.
            if (place.valid == (out[0][3] > 0.5f)) {
                report.check("aoPreviousPlace", arithmeticTolerance, Vec3{place.pixel.x, place.pixel.y, place.viewDepth}, vec3From(out[0]));
            }

            report.check("aoHistoryAge", exactTolerance, age, out[1][0]);
            report.check("aoCurrentShare", arithmeticTolerance, aoCurrentShare(age), out[1][1]);
            report.check("aoHistoryTapKept", exactTolerance, aoHistoryTapKept(in[8][0], in[8][1], in[8][2]) ? 1.0f : 0.0f, out[1][2]);
            report.check("aoTapWeight", arithmeticTolerance, aoTapWeight(fraction, static_cast<int>(in[8][3])), out[1][3]);
            const Vec4 blended = accumulatedAo(Vec4{in[6][0], in[6][1], in[6][2], in[6][3]}, Vec4{in[7][0], in[7][1], in[7][2], in[7][3]}, age);
            report.check("accumulatedAo", arithmeticTolerance, Vec3{blended.x, blended.y, blended.z}, vec3From(out[2]));
            report.check("accumulatedAo", arithmeticTolerance, blended.w, out[2][3]);
        }
    }
}

void gpuTestAmbientOcclusion(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("ambient_occlusion.glsl");
    checkOcclusion(runner, report);
    checkHistory(runner, report);
}
