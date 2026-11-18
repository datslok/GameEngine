#pragma once

#include "render/gpu/camera_culling.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class VulkanDevice;

/*
* The camera's lists built on the GPU (render/gpu/camera_culling.h): four compute passes (camera_cull_flags,
* camera_cull_scan, camera_cull_write, camera_cull_compact) write the camera's and the velocity pass's regions of the
* frame's draw list, one indirect command a group for each, and each list's commands packed style by style with their
* counts, for one vkCmdDrawIndexedIndirectCount a style (the shared mesh buffer, render/vulkan/vulkan_mesh_pool.h). Each frame in flight has its own tables in upload memory (the still part
* rewritten only when its generation changes), its own working buffers and commands in GPU memory, and a read-back
* copy of the counts and commands (and, for cull-compare, both lists) the CPU reads once the frame has finished, so
* F4's counts show a frame or two late.
*/
class VulkanCameraCulling {
public:
    VulkanCameraCulling(const VulkanDevice& device, std::uint32_t framesInFlight);

    // Between frames only: anything not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    /*
    * Record the passes into the frame's draw list (the scene's, already written by the CPU, its camera and velocity
    * regions left for the GPU: view.mainBase and view.motionBase), then make the lists readable by the vertex shaders and
    * the commands by the indirect draws. The view's candidate, group and block counts are filled in here. readLists:
    * copy both lists back too (cull-compare).
    */
    void cull(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, const CameraCullFrame& table, GpuCameraCull view,
              VkBuffer drawList, bool readLists);

    // The frame's commands, one a group in the table's group order (GpuDrawCommand each), for the groups drawn one by one.
    VkBuffer getMainCommands(std::uint32_t frameIndex) const;
    VkBuffer getMotionCommands(std::uint32_t frameIndex) const;

    // Each list's packed commands (a style's from its run's first place in the drawing order) and the counts
    // (CAMERA_CULL_DRAW_COUNTS words: the camera's styles, then the velocity pass's).
    VkBuffer getPackedMain(std::uint32_t frameIndex) const;
    VkBuffer getPackedMotion(std::uint32_t frameIndex) const;
    VkBuffer getDrawCounts(std::uint32_t frameIndex) const;

    // What a finished frame wrote: its counts, both lists' commands and, when read, their entries (from their bases).
    struct Collected {
        GpuCameraCullCounts counts;
        std::vector<GpuDrawCommand> mainCommands;
        std::vector<GpuDrawCommand> motionCommands;
        std::vector<std::uint32_t> mainEntries;
        std::vector<std::uint32_t> motionEntries;
        bool lists = false;
    };

    // Once the frame's commands have finished (its fence passed). Each is returned once; none when it culled nothing.
    std::optional<Collected> collect(std::uint32_t frameIndex);

private:
    struct PerFrame {
        // Upload memory: the tables and the view.
        VulkanBuffer records;
        VulkanBuffer levels;
        VulkanBuffer candidates;
        VulkanBuffer groups;
        VulkanBuffer view;
        VulkanBuffer order;
        std::uint64_t heldGeneration = 0; // the still part the records and candidates hold (0: none)

        // GPU memory: the passes' working buffers and the commands.
        VulkanBuffer flags;
        VulkanBuffer offsets;
        VulkanBuffer blockSums;
        VulkanBuffer counts;
        VulkanBuffer mainCommands;
        VulkanBuffer motionCommands;
        VulkanBuffer places;
        VulkanBuffer packedMain;
        VulkanBuffer packedMotion;
        VulkanBuffer drawCounts;

        // Read-back memory: counts, then both commands; both lists when asked.
        VulkanBuffer readback;
        VulkanBuffer listReadback;
        std::uint32_t groupCount = 0;
        std::uint32_t mainEntries = 0;
        std::uint32_t motionEntries = 0;
        bool pending = false;
        bool listsPending = false;
    };

    void upload(PerFrame& frame, const CameraCullFrame& table, const GpuCameraCull& view);

    const VulkanDevice& device;
    std::unique_ptr<ComputePipeline> flagPass;
    std::unique_ptr<ComputePipeline> scanPass;
    std::unique_ptr<ComputePipeline> writePass;
    std::unique_ptr<ComputePipeline> compactPass;
    std::vector<PerFrame> frames;
};
