#include "gpu_check.h"
#include "gpu_test_functions.h"

#include "render/gpu/beacon_splat.h"

#include <cstddef>
#include <string>

namespace {
    const std::string shader = gpuTestShader("beacon_splat_test");

    // Lenses from 5 cm to half a metre, a metre to 10 km away, pixels of a tenth to ten milliradians: (lens, distance, pixel).
    std::vector<GpuVec4> lensInputs() {
        CaseRandom random{41};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 300; ++i) {
            const float pixel = random.between(1e-4f, 1e-2f);
            inputs.push_back(gpuVec4(random.between(0.05f, 0.5f), random.between(1.0f, 10000.0f), pixel * pixel));
        }

        inputs.push_back(gpuVec4(0.15f, 0.0f, 1e-6f));
        return inputs;
    }

    void checkLens(GpuFunctionRunner& runner, GpuCheckReport& report) {
        const std::vector<GpuVec4> inputs = lensInputs();
        const std::vector<GpuVec4> pixels = runner.run(shader, GPU_TEST_BEACON_LENS_PIXELS, inputs, 1, 1);
        const std::vector<GpuVec4> radiances = runner.run(shader, GPU_TEST_BEACON_SPLAT_RADIANCE, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("beaconLensPixels", transcendentalTolerance, beaconLensPixels(inputs[i][0], inputs[i][1], inputs[i][2]), pixels[i][0]);
            report.check("beaconSplatRadiance", arithmeticTolerance, beaconSplatRadiance(inputs[i][0], inputs[i][1], inputs[i][2]), radiances[i][0]);
        }
    }

    // Offsets a few pixels either way, lenses from a point to ten pixels, across the tent's and the disc's edges: (offset, lens).
    void checkWeights(GpuFunctionRunner& runner, GpuCheckReport& report) {
        CaseRandom random{43};
        std::vector<GpuVec4> inputs;

        for (int i = 0; i < 1000; ++i) {
            inputs.push_back(gpuVec4(random.between(-6.0f, 6.0f), random.between(-6.0f, 6.0f), random.between(0.0f, 10.0f)));
        }

        const std::vector<GpuVec4> weights = runner.run(shader, GPU_TEST_BEACON_SPLAT_WEIGHT, inputs, 1, 1);

        for (std::size_t i = 0; i < inputs.size(); ++i) {
            report.check("beaconSplatWeight", transcendentalTolerance, beaconSplatWeight(Vec2{inputs[i][0], inputs[i][1]}, inputs[i][2]), weights[i][0]);
        }

        std::vector<GpuVec4> lenses;

        for (int i = 0; i <= 100; ++i) {
            lenses.push_back(gpuVec4(0.1f * static_cast<float>(i)));
        }

        const std::vector<GpuVec4> reached = runner.run(shader, GPU_TEST_BEACON_EXTENT_PIXELS, lenses, 1, 1);

        for (std::size_t i = 0; i < lenses.size(); ++i) {
            report.check("beaconExtentPixels", arithmeticTolerance, beaconExtentPixels(lenses[i][0]), reached[i][0]);
        }
    }
}

void gpuTestBeaconSplat(GpuFunctionRunner& runner, GpuCheckReport& report) {
    report.section("beacon_splat.glsl");
    checkLens(runner, report);
    checkWeights(runner, report);
}
