#pragma once

#include "render/gpu/camera_culling.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "render/gpu/mesh_pool.h"
#include "render/gpu/placed_drawing.h"
#include "render/gpu/twig_records.h"
#include "render/vulkan/vulkan_bone_posing.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "volk/volk.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <vector>

class VulkanDevice;

/*
* The camera's twigs culled on the GPU (ROADMAP phase 21 stage 3b-iii; render/gpu/twig_records.h), between the placed
* plants' passes and the mesh cluster passes: twig_sources (one thread: the camera's placed entries of twig parts, each
* with its part's twig set, and the cull pass's dispatch) and twig_cull (a workgroup a source's brick: each twig kept at
* its level, its entry in the region of that level's cluster count; a brick whose far node fits makes its one choice
* instead, its node's first brick putting the node's level's leaf clusters in the far region, stage 3e-iii-b). The
* cluster passes take the regions as groups and the mesh shaders build each twig's instance from its entry, and each far
* leaf cluster's leaves from the far tables.
*
* The twig tables (the placed table's twig sets' bricks and twigs, their prototypes' levels from the pool's records)
* are made once when the sets change and shared by every frame in flight; the view, groups, sources, dispatch, regions and
* entries are each frame's own. A region overflowing its room loses the rest; the room grows to fit for the frames after.
*/
class VulkanTwigDrawing {
public:
    VulkanTwigDrawing(const VulkanDevice& device, std::uint32_t framesInFlight);

    // Between frames only: anything not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    /*
    * The frame's twig tables from the placed table (after the placed drawing's prepare): none (returns false) without
    * twig sets. placement says where a prototype's clusters are in the pool, clusters is the pool's records. Remade when
    * the sets change, the GPU idle first (every frame in flight reads them). Throws std::invalid_argument for a
    * prototype that is not a chain of levels (packTwigLevels).
    */
    bool prepare(const PlacedTable& table, const std::function<MeshPlacement(MeshHandle)>& placement, std::span<const GpuMeshCluster> clusters,
                 std::uint32_t frameIndex);

    // The camera's placed plants, as the cluster passes read them.
    struct PlacedInputs {
        VkBuffer commands = VK_NULL_HANDLE;
        VkBuffer entries = VK_NULL_HANDLE;
        VkDescriptorBufferInfo partInstances{};
        VkBuffer parts = VK_NULL_HANDLE;
        VkBuffer plants = VK_NULL_HANDLE;
    };

    // Record the passes (after the placed passes and the bones', before the cluster passes): view is the camera's as the
    // cluster passes take it; bones the frame's (posed trees' bricks and twigs culled by their posed bounds). Then the
    // regions and entries readable by the cluster passes and mesh shaders, the counts copied back.
    void record(VkCommandBuffer commands, FrameDescriptors& descriptors, const PlacedInputs& placed, const BoneCullBuffers& bones, const GpuCameraCull& view,
                std::uint32_t frameIndex);

    // What the cluster passes and mesh shaders read (after record): the regions' counts and room, the entries, the twigs.
    VkBuffer getRegions(std::uint32_t frameIndex) const;
    VkBuffer getEntries(std::uint32_t frameIndex) const;
    VkBuffer getTwigs() const;

    // The far leaf clusters, leaves and cells' shapes (packTwigSets), for the cluster passes and mesh shaders; none
    // (VK_NULL_HANDLE) before the first prepare with twig sets.
    VkBuffer getFarClusters() const;
    VkBuffer getFarLeaves() const;
    VkBuffer getFarShapes() const;
    // One binding word a twig beside its record (packTwigBinding), for the bones (ROADMAP phase 21 stage 3c-iii).
    VkBuffer getTwigBindings() const;

    // The same words on the CPU, for cluster-compare.
    std::span<const std::uint32_t> getTwigBindingRecords() const;

    // A frame's region room (as prepare sized its entries), which the cluster passes' pair bound takes (twigMeshClusterPairBound).
    std::uint32_t getRoom(std::uint32_t frameIndex) const;

    // What a frame's passes counted: each region's entries written, and the cull pass's tally (F4's TWIGS line).
    struct Collected {
        std::array<std::uint32_t, TWIG_REGIONS> counts{};
        TwigTally tally;
    };

    // After a frame's fence: what it counted (none when it drew no twigs), growing the room for the frames after when a
    // region overflowed.
    std::optional<Collected> collect(std::uint32_t frameIndex);

private:
    struct PerFrame {
        VulkanBuffer view;
        VulkanBuffer groups;
        VulkanBuffer sources;
        VulkanBuffer dispatch;
        VulkanBuffer regions;
        VulkanBuffer entries;
        VulkanBuffer readback;
        bool pending = false;
        std::uint32_t room = 0; // a region's room as the frame's entries were sized (prepare)
    };

    const VulkanDevice& device;
    std::unique_ptr<ComputePipeline> sourcesPass;
    std::unique_ptr<ComputePipeline> cullPass;
    std::vector<PerFrame> frames;

    // The twig tables, and what they were made from (the sets, by address, and their prototypes' pool places).
    VulkanBuffer sets;
    VulkanBuffer bricks;
    VulkanBuffer twigs;
    VulkanBuffer twigBindings;
    std::vector<std::uint32_t> twigBindingRecords;
    VulkanBuffer prototypes;
    VulkanBuffer levels;
    VulkanBuffer farNodes;
    VulkanBuffer farLevels;
    VulkanBuffer farClusters;
    VulkanBuffer farLeaves;
    VulkanBuffer farShapes;
    std::vector<const TwigSet*> madeFrom;
    std::vector<std::uint32_t> madePlaces;
    std::uint32_t mostBricks = 0;

    // The camera's placed groups twig parts draw through (each part's every level slot).
    std::vector<std::uint32_t> twigGroups;
    std::uint32_t room = 1u << 18;
};
