#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/camera_relative.h"
#include "render/gpu/volumetric_fog.h"
#include "scene/camera.h"

#include <cstddef>
#include <string>
#include <vector>

namespace {
    const std::string shader = gpuTestShader("volumetric_fog_test");

    // A matrix as the shader takes it: four columns.
    void pushColumns(std::vector<GpuVec4>& inputs, const Mat4& matrix) {
        for (int column = 0; column < 4; ++column) {
            inputs.push_back(GpuVec4{matrix.values[0][column], matrix.values[1][column], matrix.values[2][column], matrix.values[3][column]});
        }
    }

    Fog fogFrom(const GpuVec4& medium) {
        Fog fog;
        fog.density = medium[0];
        fog.baseHeight = medium[1];
        fog.scaleHeight = medium[2];
        fog.anisotropy = medium[3];
        return fog;
    }

    /*
    * Thin and thick fog, low and high bases, rays level, rising, falling and nearly level, short and long, ending before
    * they start. Inputs per case: (density, base height, scale height, anisotropy), (origin height, direction y, start,
    * end), (source, height).
    */
    void checkMedium(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{211};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const float start = random.between(0.0f, 200.0f);
            const float directionY = i % 5 == 0 ? random.between(-1e-4f, 1e-4f) : random.between(-1.0f, 1.0f);
            inputs.push_back(GpuVec4{random.between(0.0f, 0.2f), random.between(-10.0f, 30.0f), random.between(1.0f, 100.0f), random.between(-0.9f, 0.9f)});
            inputs.push_back(GpuVec4{random.between(-5.0f, 40.0f), directionY, start, start + random.between(-10.0f, 600.0f)});
            inputs.push_back(gpuVec4(Vec3{random.between(0.0f, 4.0f), random.between(0.0f, 4.0f), random.between(0.0f, 4.0f)}, random.between(-20.0f, 80.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_FOG_MEDIUM, inputs, 3, 2);

        for (std::size_t i = 0; i < outputs.size() / 2; ++i) {
            const GpuVec4* in = &inputs[i * 3];
            const GpuVec4* out = &outputs[i * 2];
            const Fog fog = fogFrom(in[0]);
            const FogIntegration beyond = fogBeyondGrid(fog, vec3From(in[2]), in[1][0], in[1][1], in[1][2], in[1][3]);

            report.check("fogDensity", transcendentalTolerance, fogDensity(fog, in[2][3]), out[0][0]);
            report.check("fogOpticalDepth", transcendentalTolerance, fogOpticalDepth(fog, in[1][0], in[1][1], in[1][2], in[1][3]), out[0][1]);
            report.check("fogBeyondGrid", transcendentalTolerance, beyond.transmittance, out[0][2]);
            report.check("fogBeyondGrid", transcendentalTolerance, beyond.light, vec3From(out[1]));
        }
    }

    /*
    * Cameras looking every way with every lens, points all over their views from the camera out past the range, places
    * all over the grid. Inputs per case: the view-projection's columns, its inverse's columns, (point, range),
    * (coordinate, slices), (forward, history weight).
    */
    void checkGrid(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{212};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 256; ++i) {
            const Vec3 position{random.between(-50.0f, 50.0f), random.between(0.0f, 20.0f), random.between(-50.0f, 50.0f)};
            const Camera camera{position, position + random.direction(), Vec3{0.0f, 1.0f, 0.0f}, random.between(0.6f, 1.6f), random.between(1.0f, 2.4f),
                                0.1f, 1000.0f};
            const Mat4 viewProjection = cameraRelativeViewProjection(camera);
            const float range = random.between(20.0f, 300.0f);
            const Vec3 coordinate{random.between(0.0f, 1.0f), random.between(0.0f, 1.0f), random.between(0.0f, 1.0f)};
            const Vec3 point = fogVolumePoint(Vec3{random.between(0.05f, 0.95f), random.between(0.05f, 0.95f), random.between(0.0f, 1.3f)},
                                              *inverse(viewProjection), camera.getForward(), range);
            pushColumns(inputs, viewProjection);
            pushColumns(inputs, *inverse(viewProjection));
            inputs.push_back(gpuVec4(point, range));
            inputs.push_back(gpuVec4(coordinate, static_cast<float>(8 + i % 120)));
            inputs.push_back(gpuVec4(camera.getForward(), random.between(0.0f, 0.98f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_FOG_GRID, inputs, 11, 3);
        // Through a projection and back: a little more than arithmetic.
        const FloatTolerance projectionTolerance{.absolute = 1e-5f, .relative = 1e-4f};

        for (std::size_t i = 0; i < outputs.size() / 3; ++i) {
            const GpuVec4* in = &inputs[i * 11];
            const GpuVec4* out = &outputs[i * 3];
            Mat4 viewProjection;
            Mat4 inverseViewProjection;

            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    viewProjection.values[row][column] = in[column][row];
                    inverseViewProjection.values[row][column] = in[4 + column][row];
                }
            }

            const float range = in[8][3];
            const Vec3 coordinate = vec3From(in[9]);
            const int slices = static_cast<int>(in[9][3]);
            const FogSliceRead read = fogSliceRead(coordinate.z, slices);

            report.check("fogVolumeCoordinate", projectionTolerance, fogVolumeCoordinate(vec3From(in[8]), viewProjection, range), vec3From(out[0]));
            report.check("fogSliceDepth", transcendentalTolerance, fogSliceDepth(coordinate.z, range), out[0][3]);
            report.check("fogVolumePoint", projectionTolerance, fogVolumePoint(coordinate, inverseViewProjection, vec3From(in[10]), range), vec3From(out[1]));
            report.check("fogDepthSlice", transcendentalTolerance, fogDepthSlice(vec3From(in[8]).length(), range), out[1][3]);
            report.check("fogSliceRead", arithmeticTolerance, Vec3{read.z, read.share, 0.0f}, Vec3{out[2][0], out[2][1], 0.0f});
            report.check("fogHistoryWeight", exactTolerance, fogHistoryWeight(coordinate * 1.2f - Vec3{0.1f, 0.1f, 0.1f}, in[10][3]), out[2][2]);
        }
    }

    /*
    * Lights from every way seen every way with every anisotropy, slabs from clear to thick, fog so far from none to
    * nearly opaque. Inputs per case: (irradiance, anisotropy), (to the light, extinction), (view direction, length),
    * (scattered, so far's transmittance), (so far's light, 0), (colour or glass's transmittance, 0), (glass's added light, 0).
    */
    void checkLight(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{213};
        std::vector<GpuVec4> inputs;
        const auto colour = [&](float highest) {
            return Vec3{random.between(0.0f, highest), random.between(0.0f, highest), random.between(0.0f, highest)};
        };

        for (int i = 0; i < 256; ++i) {
            inputs.push_back(gpuVec4(colour(5.0f), random.between(-0.9f, 0.9f)));
            inputs.push_back(gpuVec4(random.direction(), i % 7 == 0 ? 0.0f : random.between(0.0f, 0.5f)));
            inputs.push_back(gpuVec4(random.direction(), random.between(0.0f, 40.0f)));
            inputs.push_back(gpuVec4(colour(3.0f), random.between(0.0f, 1.0f)));
            inputs.push_back(gpuVec4(colour(2.0f)));
            inputs.push_back(gpuVec4(colour(1.0f)));
            inputs.push_back(gpuVec4(colour(1.0f)));
        }

        const std::vector<GpuVec4> outputs = runner.run(shader, GPU_TEST_FOG_LIGHT, inputs, 7, 6);

        for (std::size_t i = 0; i < outputs.size() / 6; ++i) {
            const GpuVec4* in = &inputs[i * 7];
            const GpuVec4* out = &outputs[i * 6];
            const FogIntegration soFar{vec3From(in[4]), in[3][3]};
            FogIntegration slab = soFar;
            fogIntegrateSlice(slab, vec3From(in[3]), in[1][3], in[2][3]);
            const FogIntegration followed = fogFollow(soFar, slab);

            report.check("fogInScattering", arithmeticTolerance, fogInScattering(vec3From(in[0]), vec3From(in[1]), vec3From(in[2]), in[0][3]), vec3From(out[0]));
            report.check("fogAmbient", arithmeticTolerance, fogAmbient(vec3From(in[0]), vec3From(in[3])), vec3From(out[1]));
            report.check("fogIntegrateSlice", transcendentalTolerance, slab.light, vec3From(out[2]));
            report.check("fogIntegrateSlice", transcendentalTolerance, slab.transmittance, out[2][3]);
            report.check("fogFollow", transcendentalTolerance, followed.light, vec3From(out[3]));
            report.check("fogFollow", transcendentalTolerance, followed.transmittance, out[3][3]);
            report.check("fogComposite", arithmeticTolerance, fogComposite(vec3From(in[5]), soFar), vec3From(out[4]));
            report.check("fogOverGlass", arithmeticTolerance, fogOverGlass(vec3From(in[6]), vec3From(in[5]), soFar), vec3From(out[5]));
        }
    }
}

void gpuTestVolumetricFog(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("volumetric_fog.glsl");
    checkMedium(runner, report);
    checkGrid(runner, report);
    checkLight(runner, report);
}
