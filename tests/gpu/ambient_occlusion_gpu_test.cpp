#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/ambient_occlusion.h"

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

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_AMBIENT_OCCLUSION, inputs, 11, 6);
        // acos near its ends and the cross products near parallel steps can lose digits.
        const FloatTolerance angleTolerance{.absolute = 1e-4f, .relative = 1e-3f};

        for (std::size_t i = 0; i < outputs.size() / 6; ++i) {
            const GpuVec4* in = &inputs[i * 11];
            const GpuVec4* out = &outputs[i * 6];
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
        }
    }
}

void gpuTestAmbientOcclusion(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("ambient_occlusion.glsl");
    checkOcclusion(runner, report);
}
