#include "gpu_function_runner.h"
#include "gpu_test_functions.h"

#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_memory.h"

#include <array>
#include <cstring>
#include <stdexcept>

namespace {
    // Must match the push constant block in tests/gpu/shaders/gpu_test.glsl.
    struct TestPushConstants {
        std::uint32_t count;
        std::uint32_t function;
        std::uint32_t inputsPerCase;
        std::uint32_t outputsPerCase;
    };

    static_assert(sizeof(TestPushConstants) == 16, "TestPushConstants must match the shaders' TestCase block");

    constexpr std::uint32_t buffersPerShader = 2;
}

/*
* One descriptor set at a time (each run resets the pool before allocating), holding the two storage buffers.
*/
GpuFunctionRunner::GpuFunctionRunner(const VulkanDevice& device):
    device(device),
    commands(device)
{
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffersPerShader};

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = 1;
    info.poolSizeCount = 1;
    info.pPoolSizes = &size;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorPool(device.getDevice(), &info, nullptr, &pool), "Descriptor pool creation failed");
    descriptorPool = OwnedHandle<VkDescriptorPool>{device.getDevice(), pool, vkDestroyDescriptorPool};
}

GpuFunctionRunner::~GpuFunctionRunner() = default;

ComputePipeline& GpuFunctionRunner::pipelineFor(const std::string& shader) {
    auto found = pipelines.find(shader);

    if (found == pipelines.end()) {
        ComputePipelineDescription description;
        description.shader = shader.c_str();
        description.storageBuffers = buffersPerShader;
        description.pushConstantBytes = sizeof(TestPushConstants);
        found = pipelines.emplace(shader, std::make_unique<ComputePipeline>(device, description)).first;
    }

    return *found->second;
}

/*
* Inputs go into an upload buffer and results come back through a read-back buffer, both mapped, so no copies are
* needed. Two rules make the numbers reach the other side:
* - CPU writes before vkQueueSubmit are visible to the GPU without a barrier (submitting does that), once flushed
*   where the memory is not coherent.
* - GPU writes are not visible to the CPU until a barrier moves them from the shader's caches to memory the host can
*   read (compute shader writes before host reads), and the CPU waits for the fence (ImmediateCommands::run does), then
*   invalidates its view where the memory is not coherent.
*/
std::vector<GpuVec4> GpuFunctionRunner::run(const std::string& shader, std::uint32_t function, const std::vector<GpuVec4>& inputs,
                                            std::uint32_t inputsPerCase, std::uint32_t outputsPerCase) {
    if (inputsPerCase == 0 || outputsPerCase == 0 || inputs.size() % inputsPerCase != 0) {
        throw std::invalid_argument("GPU test inputs must be whole cases of at least one vec4 each");
    }

    const std::uint32_t count = static_cast<std::uint32_t>(inputs.size() / inputsPerCase);

    if (count == 0) {
        return {};
    }

    ComputePipeline& pipeline = pipelineFor(shader);
    const VkDeviceSize inputBytes = inputs.size() * sizeof(GpuVec4);
    const VkDeviceSize outputBytes = static_cast<VkDeviceSize>(count) * outputsPerCase * sizeof(GpuVec4);

    VulkanBuffer inputBuffer{device.getAllocator(), inputBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload};
    VulkanBuffer outputBuffer{device.getAllocator(), outputBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Readback};
    std::memcpy(inputBuffer.getMapped(), inputs.data(), inputBytes);
    inputBuffer.flush();

    const VkDevice handle = device.getDevice();
    vulkanCheck(vkResetDescriptorPool(handle, descriptorPool.get(), 0), "Descriptor pool reset failed");

    const VkDescriptorSetLayout setLayout = pipeline.getResourceLayout();
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = descriptorPool.get();
    allocateInfo.descriptorSetCount = 1;
    allocateInfo.pSetLayouts = &setLayout;

    VkDescriptorSet set = VK_NULL_HANDLE;
    vulkanCheck(vkAllocateDescriptorSets(handle, &allocateInfo, &set), "Descriptor set allocation failed");

    const std::array<VkDescriptorBufferInfo, buffersPerShader> bufferInfos{
        VkDescriptorBufferInfo{inputBuffer.get(), 0, VK_WHOLE_SIZE},
        VkDescriptorBufferInfo{outputBuffer.get(), 0, VK_WHOLE_SIZE},
    };
    std::array<VkWriteDescriptorSet, buffersPerShader> writes{};

    for (std::uint32_t binding = 0; binding < buffersPerShader; ++binding) {
        writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[binding].dstSet = set;
        writes[binding].dstBinding = binding;
        writes[binding].descriptorCount = 1;
        writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[binding].pBufferInfo = &bufferInfos[binding];
    }

    vkUpdateDescriptorSets(handle, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    const TestPushConstants constants{count, function, inputsPerCase, outputsPerCase};

    commands.run([&](VkCommandBuffer commandBuffer) {
        device.beginLabel(commandBuffer, shader.c_str());
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(commandBuffer, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
        vkCmdDispatch(commandBuffer, workgroupCount(count, GPU_TEST_GROUP_SIZE), 1, 1);

        VkMemoryBarrier2 toHost{};
        toHost.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &toHost;
        vkCmdPipelineBarrier2(commandBuffer, &dependency);
        device.endLabel(commandBuffer);
    });

    outputBuffer.invalidate();
    std::vector<GpuVec4> outputs(static_cast<std::size_t>(count) * outputsPerCase);
    std::memcpy(outputs.data(), outputBuffer.getMapped(), outputBytes);
    return outputs;
}
