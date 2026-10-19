#pragma once

#include "render/vulkan/owned_handle.h"

#include "volk/volk.h"

#include <cstdint>

class VulkanDevice;

// What a compute shader reads and writes: storage buffers at set 0, bindings 0.. storageBuffers - 1, and a few bytes
// of push constants (written into the command buffer itself, the cheapest way to pass a handful of numbers).
struct ComputePipelineDescription {
    const char* shader = nullptr;
    std::uint32_t storageBuffers = 0;
    std::uint32_t pushConstantBytes = 0;

    // A pipeline layout made elsewhere and shared (the scene's, render/vulkan/scene_layout.h), used instead of building
    // one from storageBuffers and pushConstantBytes, so the shader reads the frame's scene set as the passes do. Not
    // owned: it must outlive the pipeline.
    VkPipelineLayout sharedLayout = VK_NULL_HANDLE;
};

/*
* A compute pipeline: one compute shader compiled by the driver, with the layout of what it reads and writes. Unlike a
* graphics pipeline there is no fixed-function state (no vertices, rasteriser, depth or blending): the shader runs once
* per invocation, in workgroups, and does all its input and output through the resources bound to it. The first users
* are the GPU tests (tests/gpu) and the cluster pass (cluster_lights.comp); DDGI's probe updates and the cloth solver
* follow.
*/
class ComputePipeline {
public:
    ComputePipeline(const VulkanDevice& device, const ComputePipelineDescription& description);

    VkPipeline get() const;
    VkPipelineLayout getLayout() const;

    // Set 0's layout, for allocating the descriptor set that points at the storage buffers (none with a shared layout).
    VkDescriptorSetLayout getResourceLayout() const;

    std::uint32_t getPushConstantBytes() const;

private:
    std::uint32_t pushConstantBytes = 0;
    VkPipelineLayout sharedLayout = VK_NULL_HANDLE;
    OwnedHandle<VkDescriptorSetLayout> resourceLayout;
    OwnedHandle<VkPipelineLayout> layout;
    OwnedHandle<VkPipeline> pipeline;

    VkPipelineLayout createLayout(VkDevice handle, const ComputePipelineDescription& description);
};
