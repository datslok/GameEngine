#pragma once

#include "render/gpu/camera_culling.h"
#include "render/gpu/mesh_cluster_culling_shared.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_lasting_buffer.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

class VulkanDevice;

/*
* What a pass draws: the CPU's batches, or, while the GPU culls (render/gpu/camera_culling.h), the frame's groups: each
* style's pooled groups by one multi-draw of the packed commands (packed, their counts in counts from word countOffset),
* then the groups with buffers of their own one by one from commands. A view's commands start at group commandFirst of
* both command buffers.
*/
struct PassDraws {
    const std::vector<DrawBatch>* batches = nullptr;
    const CameraCullFrame* groups = nullptr;
    VkBuffer commands = VK_NULL_HANDLE;
    VkBuffer packed = VK_NULL_HANDLE;
    VkBuffer counts = VK_NULL_HANDLE;
    std::uint32_t countOffset = 0;
    std::uint32_t commandFirst = 0;

    // The camera's mesh clusters' draws (render/vulkan/vulkan_mesh_cluster_culling.h, a VkDrawMeshTasksIndirectCommandEXT a
    // list and style), drawn by mesh shaders in place of the pooled groups' multi-draws; none to draw the groups.
    // meshLists: the lists drawn, a bit each (1 << MESH_CLUSTER_EARLY, 1 << MESH_CLUSTER_LATE); meshOnly: nothing else
    // (the late list's depth, after the early list's prepass drew the rest).
    VkBuffer meshTasks = VK_NULL_HANDLE;
    std::uint32_t meshLists = 1u << MESH_CLUSTER_EARLY;
    bool meshOnly = false;

    // The pyramid the mesh shaders test each far leaf against (MESH_CLUSTER_LEAF_TEST_*, stage 3e-v-b).
    std::uint32_t farLeafTest = MESH_CLUSTER_LEAF_TEST_NONE;
};

/*
* The views' lists built on the GPU (render/gpu/camera_culling.h): the segment passes (camera_cull_segments,
* camera_cull_segment_scan, camera_cull_segment_list: chunks tested, visible candidates found), then camera_cull_flags
* (indirect, over the visible candidates), camera_cull_scan, camera_cull_write (indirect) and camera_cull_compact, each
* one dispatch over every view (the camera first, then
* the probe faces and the mirror drawn this frame: the dispatch's second dimension), write each view's region of the
* frame's draw list (and the camera's velocity region), one indirect command a group for each, and each list's commands
* packed style by style with their counts, for one vkCmdDrawIndexedIndirectCount a style (the shared mesh buffer,
* render/vulkan/vulkan_mesh_pool.h). Each view works in its own slices of the working buffers (placeCullView). The
* records and candidates are kept in GPU memory every frame reads (render/vulkan/vulkan_lasting_buffer.h): the still
* records copied where repacked, the still candidates when built afresh, the moving ones every frame. Each frame in
* flight has its own other tables in upload memory, its own working buffers and commands in GPU memory, and a read-back copy of the counts and commands (and, for
* cull-compare, the lists) the CPU reads once the frame has finished, so F4's counts show a frame or two late.
*/
class VulkanCameraCulling {
public:
    VulkanCameraCulling(const VulkanDevice& device, std::uint32_t framesInFlight);

    // Between frames only: anything not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    /*
    * Record the passes into the frame's draw list (the scene's, already written by the CPU, each view's regions left for
    * the GPU: each view's mainBase and motionBase), then make the lists readable by the vertex shaders and the
    * commands by the indirect draws. views: the camera first, then any number; each one's place (placeCullView) is
    * filled in here. readLists: copy the lists back too (cull-compare).
    */
    void cull(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, const CameraCullFrame& table,
              std::span<const GpuCameraCull> views, VkBuffer drawList, bool readLists);

    // What a view's list draws (the frame's commands for that view), and its second list (the camera's velocity pass, a
    // shadow tile's moved casters).
    PassDraws mainDraws(std::uint32_t frameIndex, const CameraCullFrame& table, std::uint32_t view) const;
    PassDraws motionDraws(std::uint32_t frameIndex, const CameraCullFrame& table, std::uint32_t view) const;

    // What a finished frame wrote, by view: its counts, both lists' commands and, when read, both lists' entries (from
    // their bases).
    struct Collected {
        std::vector<GpuCameraCullCounts> counts;
        std::vector<std::vector<GpuDrawCommand>> mainCommands;
        std::vector<std::vector<GpuDrawCommand>> motionCommands;
        std::vector<std::vector<std::uint32_t>> mainEntries;
        std::vector<std::vector<std::uint32_t>> motionEntries;
        bool lists = false;
    };

    // Once the frame's commands have finished (its fence passed). Each is returned once; none when it culled nothing.
    std::optional<Collected> collect(std::uint32_t frameIndex);

    // Bytes the last cull copied into the kept records and candidates.
    std::size_t getCopiedBytes() const;

    // What a frame's cull wrote and read that later passes read too (the mesh clusters' culling): the views, the drawing
    // order, the groups, both lists' unpacked commands, every draw's record.
    struct Outputs {
        VkBuffer views = VK_NULL_HANDLE;
        VkBuffer order = VK_NULL_HANDLE;
        VkBuffer groups = VK_NULL_HANDLE;
        VkBuffer mainCommands = VK_NULL_HANDLE;
        VkBuffer motionCommands = VK_NULL_HANDLE;
        VkBuffer records = VK_NULL_HANDLE;
    };

    Outputs outputs(std::uint32_t frameIndex) const;

    // A view as the frame's cull placed it (its runs and slices filled in).
    const GpuCameraCull& placedView(std::uint32_t frameIndex, std::uint32_t view) const;

private:
    struct PerFrame {
        // Upload memory: the levels, groups, views and drawing order.
        VulkanBuffer levels;
        VulkanBuffer groups;
        VulkanBuffer views;
        VulkanBuffer order;

        // GPU memory: the passes' working buffers and the commands, a slice a view; each view's work, each batch's
        // indirect dispatches.
        VulkanBuffer segmentOffsets;
        VulkanBuffer segmentBlockSums;
        VulkanBuffer visibleSegments;
        VulkanBuffer work;
        VulkanBuffer dispatches;
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

        // Read-back memory: counts, then the commands; the lists when asked.
        VulkanBuffer readback;
        VulkanBuffer listReadback;
        // The views as placed this frame (placeCullView).
        std::vector<GpuCameraCull> placed;
        std::uint32_t groupCount = 0;
        std::uint32_t viewCount = 0;
        std::uint32_t listEntries = 0;
        bool pending = false;
        bool listsPending = false;
    };

    void upload(VkCommandBuffer commands, std::uint32_t frameIndex, const CameraCullFrame& table, std::span<const GpuCameraCull> views);

    const VulkanDevice& device;
    std::unique_ptr<ComputePipeline> segmentPass;
    std::unique_ptr<ComputePipeline> segmentScanPass;
    std::unique_ptr<ComputePipeline> segmentListPass;
    std::unique_ptr<ComputePipeline> flagPass;
    std::unique_ptr<ComputePipeline> scanPass;
    std::unique_ptr<ComputePipeline> writePass;
    std::unique_ptr<ComputePipeline> compactPass;
    std::vector<PerFrame> frames;

    // Every draw's record, the still chunks, every candidate and segment, kept; the still part's generation they hold
    // (0: none).
    std::unique_ptr<VulkanLastingBuffer> records;
    std::unique_ptr<VulkanLastingBuffer> chunks;
    std::unique_ptr<VulkanLastingBuffer> candidates;
    std::unique_ptr<VulkanLastingBuffer> segments;
    std::uint64_t heldGeneration = 0;
};
