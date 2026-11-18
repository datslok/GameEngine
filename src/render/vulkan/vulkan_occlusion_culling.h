#pragma once

#include "render/gpu/occlusion_culling.h"
#include "render/gpu/upscaling.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_depth_pyramid.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class VulkanDevice;

/*
* Occlusion culling on the GPU (render/gpu/occlusion_culling.h). First stage: after the depth prepass, the farthest-depth
* pyramid is built from its depth and occlusion_count.comp counts how many of the camera's draws it would hide; the
* counts are copied into read-back memory the CPU reads once the frame has finished (as VulkanDepthRange does), so F4
* shows a frame or two late. Each frame in flight has its own batch table and read-back copy.
*/
class VulkanOcclusionCulling {
public:
    // nearest: exact texels, clamped. texturesLayout and sceneLayout: the scene's sets 0 and 1, which the pass reads
    // the instances and the draw list from.
    VulkanOcclusionCulling(const VulkanDevice& device, VkSampler nearest, VkDescriptorSetLayout texturesLayout, VkDescriptorSetLayout sceneLayout,
                           std::uint32_t framesInFlight);

    // Between frames only: any count not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    // The pyramid at the render size; the GPU must be idle (the renderer waits).
    void ensureSize(RenderSize render);

    // Record the pyramid from the prepass's depth (left readable by compute) and the count of the camera's batches.
    void count(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth, VkDescriptorSet textures,
               VkDescriptorSet scene, const std::vector<GpuOcclusionBatch>& batches, const OcclusionView& view, const Vec3& cameraOrigin);

    // Once the frame's commands have finished (its fence passed): what it counted. Each count is returned once; none
    // when the frame counted nothing.
    std::optional<OcclusionCounts> collect(std::uint32_t frameIndex);

private:
    struct PerFrame {
        VulkanBuffer batches; // upload memory, grown when a frame has more
        VulkanBuffer readback;
        bool pending = false;
    };

    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<VulkanDepthPyramid> pyramid;
    std::unique_ptr<ComputePipeline> pipeline;

    // Where the pass adds up its counts: in GPU memory, since every workgroup ends with atomics on it.
    VulkanBuffer counts;
    std::vector<PerFrame> frames;
};
