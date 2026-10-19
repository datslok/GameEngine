#pragma once

#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_pipeline.h"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class VulkanDevice;
class ComputePipeline;

// Four floats, a GLSL vec4: the unit every test shader reads and writes (16 bytes in std430 and in C++ alike).
using GpuVec4 = std::array<float, 4>;

/*
* Runs one function of a test shader (tests/gpu/shaders) on the GPU over a list of cases and returns its results: the
* inputs go into one storage buffer, inputsPerCase vec4s per case, the shader runs once per case and writes
* outputsPerCase vec4s per case into another, which is read back. Each shader is loaded once and kept.
*/
class GpuFunctionRunner {
public:
    explicit GpuFunctionRunner(const VulkanDevice& device);
    ~GpuFunctionRunner();

    GpuFunctionRunner(const GpuFunctionRunner&) = delete;
    GpuFunctionRunner& operator=(const GpuFunctionRunner&) = delete;

    // shader is a compiled test shader's path; function one of its GPU_TEST_ numbers (tests/gpu/gpu_test_functions.h).
    // inputs.size() must be a whole number of cases.
    std::vector<GpuVec4> run(const std::string& shader, std::uint32_t function, const std::vector<GpuVec4>& inputs,
                             std::uint32_t inputsPerCase, std::uint32_t outputsPerCase);

private:
    const VulkanDevice& device;
    ImmediateCommands commands;
    OwnedHandle<VkDescriptorPool> descriptorPool;
    std::map<std::string, std::unique_ptr<ComputePipeline>> pipelines;

    ComputePipeline& pipelineFor(const std::string& shader);
};
