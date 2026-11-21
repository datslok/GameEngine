#pragma once

#include "render/gpu/camera_culling.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class VulkanDevice;

/*
* The camera's mesh clusters culled on the GPU (render/gpu/mesh_cluster_culling.h), after the camera's culling has
* written its list: mesh_cluster_count (one workgroup: the pairs to test, each style from a block's first),
* mesh_cluster_test (indirect: each block's survivors), mesh_cluster_scan (one workgroup: the blocks' first places, the
* totals and runs) and mesh_cluster_write (indirect: each survivor at its place). Each frame in flight has its own view,
* working buffers and list, and a read-back copy of the totals (and, for cluster-compare, the list), read once its fence
* has passed. A list overflowing its room is cut, and the room grows to fit for the frames after.
*/
class VulkanMeshClusterCulling {
public:
    VulkanMeshClusterCulling(const VulkanDevice& device, std::uint32_t framesInFlight);

    // Between frames only: anything not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    /*
    * What the passes read: the camera's view as its culling placed it (placeCullView), with the wind's sway; the camera
    * culling's drawing order, groups, unpacked commands and records (render/vulkan/vulkan_camera_culling.h); the frame's
    * draw list and instances; the cluster pool. pooled: the places in the drawing order's pooled runs; pairBound:
    * meshClusterPairBound.
    */
    struct Inputs {
        GpuCameraCull view{};
        VkBuffer order = VK_NULL_HANDLE;
        VkBuffer groups = VK_NULL_HANDLE;
        VkBuffer commands = VK_NULL_HANDLE;
        VkBuffer records = VK_NULL_HANDLE;
        VkBuffer drawList = VK_NULL_HANDLE;
        VkDescriptorBufferInfo instances{};
        VkBuffer clusters = VK_NULL_HANDLE;
        std::uint32_t pooled = 0;
        std::uint64_t pairBound = 0;
    };

    // Where the frame's list and totals will be, for the mesh shaders that draw them (frame_data.glsl): made before the
    // frame's scene is written, so cull keeps them where they are.
    struct ListAddresses {
        VkDeviceAddress items = 0;
        VkDeviceAddress totals = 0;
    };

    ListAddresses prepare(std::uint32_t frameIndex);

    // Record the passes (after prepare); readItems: copy the list back too (cluster-compare).
    void cull(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, const Inputs& inputs, bool readItems);

    // The frame's mesh shader draw for each style (VkDrawMeshTasksIndirectCommandEXT, MESH_CLUSTER_STYLES of them),
    // written by the scan pass and ready for the indirect draws.
    VkBuffer getTaskCommands(std::uint32_t frameIndex) const;

    // What a finished frame wrote: its totals (MESH_CLUSTER_TOTAL_*), and, when read, its pairs (within the room).
    struct Collected {
        std::array<std::uint32_t, MESH_CLUSTER_TOTAL_WORDS> totals{};
        std::vector<GpuMeshClusterItem> items;
        bool itemsRead = false;
    };

    // Once the frame's commands have finished (its fence passed). Each is returned once; none when it culled nothing.
    std::optional<Collected> collect(std::uint32_t frameIndex);

    // The room for pairs.
    std::uint32_t getRoom() const;

private:
    struct PerFrame {
        // Upload memory: the view, and setup (places, room for block sums, room for pairs).
        VulkanBuffer view;
        VulkanBuffer setup;

        // GPU memory: the passes' working buffers, the dispatch, the totals and the list.
        VulkanBuffer workEnds;
        VulkanBuffer work;
        VulkanBuffer dispatch;
        VulkanBuffer blockCounts;
        VulkanBuffer blockFirst;
        VulkanBuffer totals;
        VulkanBuffer items;
        VulkanBuffer taskCommands;

        // Read-back memory: the totals, then the list when asked.
        VulkanBuffer readback;
        std::uint32_t room = 0;
        bool pending = false;
        bool itemsPending = false;
    };

    const VulkanDevice& device;
    std::unique_ptr<ComputePipeline> countPass;
    std::unique_ptr<ComputePipeline> testPass;
    std::unique_ptr<ComputePipeline> scanPass;
    std::unique_ptr<ComputePipeline> writePass;
    std::vector<PerFrame> frames;
    std::uint32_t room = smallestMeshClusterRoom;
};
