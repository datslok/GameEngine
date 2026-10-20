#include "gpu_check.h"

#include "render/vulkan/vulkan_device.h"

#include <exception>
#include <iostream>

/*
* make gputest: runs every shader library's functions on the GPU over fixed inputs and compares the results with their
* C++ mirrors, which make test checks against the physics. Needs a GPU but no window. The validation layers are on, and
* any error they report fails the run, as in the golden images.
*/
int main() {
    try {
        VulkanDevice device{nullptr, true};
        std::cout << "GPU tests on " << device.getName() << '\n';

        GpuCheckReport report;
        {
            GpuFunctionRunner runner{device};
            gpuTestBrdf(runner, report);
            gpuTestNormalMapping(runner, report);
            gpuTestLighting(runner, report);
            gpuTestSoftShadow(runner, report);
            gpuTestToneMapping(runner, report);
            gpuTestBloom(runner, report);
            gpuTestDither(runner, report);
            gpuTestOverlay(runner, report);
            gpuTestStars(runner, report);
            gpuTestClusters(runner, report);
            gpuTestCascades(runner, report);
            gpuTestDdgiSampling(runner, report);
            gpuTestProbeRelocation(runner, report);
            gpuTestAtmosphere(device, runner, report);
            gpuTestDepthRange(device, report);
            gpuTestRayTracing(device, runner, report);
            gpuTestDdgi(device, runner, report);
            report.print();
        }

        const std::uint32_t validationErrors = device.getValidationErrorCount();

        if (validationErrors > 0) {
            std::cout << "FAIL: the validation layers reported " << validationErrors << " errors\n";
            return 1;
        }

        if (report.failedGroups() > 0) {
            std::cout << "FAIL: " << report.failedGroups() << " shader functions differ from their C++ mirrors\n";
            return 1;
        }

        std::cout << "All shader functions match their C++ mirrors.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cout << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
