#pragma once

#include "math/mat4.h"
#include "math/vec3.h"
#include "render/gpu/camera_culling.h"
#include "render/gpu/mesh_cluster_culling.h"
#include "render/gpu/upscaling.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/render_pass.h"
#include "render/vulkan/vulkan_bone_posing.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_depth_pyramid.h"
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
* working buffers and lists, and a read-back copy of the totals (and, for cluster-compare, the lists), read once its
* fence has passed. A list overflowing its room is cut, and the room grows to fit for the frames after.
*
* With occlusion (stage 1d, two passes): cull makes the early list against last frame's depth pyramid (the history);
* after the early list's depth is drawn, cullLate builds this frame's pyramid from it and makes the late list (what the
* history hid that this one does not); after the late list's depth is drawn, keepDepth builds the history for the next
* frame. Both pyramids keep the farthest depth (VulkanDepthPyramid), at the render size. The work and dispatch of cull's
* count pass serve both lists.
*/
class VulkanMeshClusterCulling {
public:
    // nearest: exact texels, clamped, every level (for the depth and the pyramids).
    VulkanMeshClusterCulling(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight);

    // Between frames only: anything not yet collected is dropped.
    void setFramesInFlight(std::uint32_t count);

    // The render size the pyramids take; the GPU must be idle (the renderer waits). The history is lost.
    void ensureSize(RenderSize render);

    /*
    * The view's placed plants (render/vulkan/vulkan_placed_drawing.h), each buffer the passes' own record when there
    * are none: every view's commands, the camera's first, and the view's first among them, the placed groups, every
    * view's entries, the parts' instances and records, the slots' plants.
    */
    struct PlacedInputs {
        VkBuffer commands = VK_NULL_HANDLE;
        VkBuffer groups = VK_NULL_HANDLE;
        VkBuffer entries = VK_NULL_HANDLE;
        VkDescriptorBufferInfo partInstances{};
        VkBuffer parts = VK_NULL_HANDLE;
        VkBuffer plants = VK_NULL_HANDLE;
        std::uint32_t commandFirst = 0;
    };

    /*
    * What the passes read: the camera's view as its culling placed it (placeCullView), with the wind's sway, its runs
    * the order's; the order (meshClusterDrawOrder, uploaded here) over drawGroups of the camera culling's groups, whose
    * groups, unpacked commands and records are given (render/vulkan/vulkan_camera_culling.h); the frame's draw list and
    * instances; the cluster pool; the placed plants. pairBound: meshClusterPairBound and placedMeshClusterPairBound.
    * occlusion: two lists; viewProjection: this frame's camera-relative view-projection as the depth is drawn
    * (jittered), from origin. bones: the trees posed this frame, culled by their posed bounds (ROADMAP phase 21 3c-iv-b).
    */
    // The camera's twigs (render/vulkan/vulkan_twig_drawing.h), the passes' own record each without: the regions' counts
    // and room, the entries, the twigs.
    struct TwigInputs {
        VkBuffer regions = VK_NULL_HANDLE;
        VkBuffer entries = VK_NULL_HANDLE;
        VkBuffer twigs = VK_NULL_HANDLE;
        VkBuffer farClusters = VK_NULL_HANDLE;
    };

    struct Inputs {
        GpuCameraCull view{};
        std::vector<std::uint32_t> order;
        std::uint32_t drawGroups = 0;
        VkBuffer groups = VK_NULL_HANDLE;
        VkBuffer commands = VK_NULL_HANDLE;
        VkBuffer records = VK_NULL_HANDLE;
        VkBuffer drawList = VK_NULL_HANDLE;
        VkDescriptorBufferInfo instances{};
        VkBuffer clusters = VK_NULL_HANDLE;
        PlacedInputs placed;
        TwigInputs twigs;
        BoneCullBuffers bones;
        VkBuffer lianas = VK_NULL_HANDLE;       // the lianas' records (none: no lianas)
        VkBuffer lianaMotions = VK_NULL_HANDLE; // their anchors' motions this frame (VulkanLianaTrees::getMotions; none: still)
        VkDescriptorBufferInfo terrainPatches{}; // the frame's terrain patches (none: no terrain)
        VkBuffer terrainHeights = VK_NULL_HANDLE;  // the land's heights and pyramids (none: no terrain)
        std::uint64_t pairBound = 0;
        bool occlusion = false;
        Mat4 viewProjection;
        Vec3 origin;
    };

    // Where the frame's lists and totals will be, for the mesh shaders that draw them (frame_data.glsl): made before the
    // frame's scene is written, so cull keeps them where they are.
    struct ListAddresses {
        VkDeviceAddress items = 0;
        VkDeviceAddress totals = 0;
    };

    ListAddresses prepare(std::uint32_t frameIndex);

    // Record the early list's passes (after prepare); readItems: copy the lists back too (cluster-compare). Without
    // occlusion this is the frame's only list, and its totals (and list) are copied back here.
    void cull(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, const Inputs& inputs, bool readItems);

    // With occlusion, after the early list's depth is drawn: this frame's pyramid from the depth image, then the late
    // list's passes, and both lists' totals (and lists) copied back. The depth ends readable by compute.
    void cullLate(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth);

    // With occlusion, after the late list's depth is drawn: the history for the next frame, from the depth image.
    void keepDepth(VkCommandBuffer commands, FrameDescriptors& descriptors, VulkanImage& depth);

    // With occlusion, after the main pass: the far leaf tally its mesh shaders added, copied back with the totals.
    void readLeafTally(VkCommandBuffer commands, std::uint32_t frameIndex);

    // The frame's mesh shader draws for each list and style (VkDrawMeshTasksIndirectCommandEXT, MESH_CLUSTER_LISTS times
    // MESH_CLUSTER_STYLES of them, the early list's first), written by the scan passes and ready for the indirect draws.
    VkBuffer getTaskCommands(std::uint32_t frameIndex) const;

    // What a finished frame wrote: each list's totals (MESH_CLUSTER_TOTAL_*; the late list's all zero without
    // occlusion), and, when read, its pairs (within the room).
    struct Collected {
        std::array<std::array<std::uint32_t, MESH_CLUSTER_TOTAL_WORDS>, MESH_CLUSTER_LISTS> totals{};
        std::array<std::vector<GpuMeshClusterItem>, MESH_CLUSTER_LISTS> items;
        bool itemsRead = false;
        bool occlusion = false;

        // With occlusion, the far leaves the main pass's mesh shaders tested and hid, then the late prepass's
        // (MESH_CLUSTER_LEAF_TALLY_*; zero while far leaf occlusion is off).
        std::array<std::uint32_t, MESH_CLUSTER_LEAF_TALLY_WORDS> leafTally{};
    };

    // Once the frame's commands have finished (its fence passed). Each is returned once; none when it culled nothing.
    std::optional<Collected> collect(std::uint32_t frameIndex);

    // The room for pairs, each list's.
    std::uint32_t getRoom() const;

    /*
    * For a pass of another system's over the frame's early list once cull made it (the sun's virtual shadow casters'
    * expanding, render/vulkan/vulkan_virtual_shadows.h): the bindings every pass shares (mesh_cluster_pass.glsl, from
    * what cull was given), then the list's items and totals. The list and its task commands are left for the mesh
    * shaders and indirect reads; such a pass makes them visible to compute itself.
    */
    std::vector<ComputeBinding> listPassBindings(std::uint32_t frameIndex) const;

    // The occlusion pyramids for the mesh shaders' far leaves (stage 3e-v-b), made now if not yet; none before the render
    // size is known. This frame's early list's, and the whole frame's (the next frame's history). Readable by mesh shaders
    // whenever they draw, but read only by a draw told to test against one, after cullLate or keepDepth built it.
    std::optional<SampledImage> sampledEarlyDepth();
    std::optional<SampledImage> sampledWholeDepth();

private:
    struct PerFrame {
        // What cull was given, for the late list's passes.
        Inputs inputs;

        // Upload memory: the view, the order, setup (places, room for block sums, room for pairs, draw groups), the
        // occlusion record, and one unused record for each placed buffer when there are no placed plants.
        VulkanBuffer view;
        VulkanBuffer order;
        VulkanBuffer setup;
        VulkanBuffer occlusionRecord;
        VulkanBuffer noPlaced;

        // GPU memory: the passes' working buffers, the dispatch, both lists' totals, items and task commands.
        VulkanBuffer workEnds;
        VulkanBuffer work;
        VulkanBuffer dispatch;
        VulkanBuffer blockCounts;
        VulkanBuffer blockFirst;
        VulkanBuffer totals;
        VulkanBuffer items;
        VulkanBuffer taskCommands;

        // Read-back memory: both lists' totals, then the lists when asked.
        VulkanBuffer readback;
        std::uint32_t room = 0;
        bool pending = false;
        bool itemsPending = false;
        bool occlusion = false;
    };

    // The pass's bindings for this frame's list: the shared buffers, its own, and the two pyramids when it tests pairs.
    std::vector<ComputeBinding> passBindings(const PerFrame& frame, std::initializer_list<VkBuffer> own, bool pyramids) const;

    // Record one list's test, scan and write passes (the count pass's work and dispatch ready), and make it ready to draw.
    void recordList(VkCommandBuffer commands, FrameDescriptors& descriptors, PerFrame& frame, std::uint32_t list);

    // Copy the frame's totals (and lists, when asked) back for the CPU.
    void readBack(VkCommandBuffer commands, PerFrame& frame);

    // A pyramid image readable by the passes, built or not (an unbuilt one is never read: its flags say so).
    void makeReadable(VkCommandBuffer commands, VulkanDepthPyramid& pyramid, bool& built);

    // The pyramids made at the render size, once it is known (false before).
    bool makePyramids();

    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> countPass;
    std::unique_ptr<ComputePipeline> testPass;
    std::unique_ptr<ComputePipeline> scanPass;
    std::unique_ptr<ComputePipeline> writePass;
    std::vector<PerFrame> frames;
    std::uint32_t room = smallestMeshClusterRoom;

    // The pyramids, made at the first cull: the history (last frame's whole depth) and this frame's from the early list.
    std::unique_ptr<VulkanDepthPyramid> history;
    std::unique_ptr<VulkanDepthPyramid> earlyDepth;
    RenderSize pyramidSize{};
    bool pyramidsMade = false;
    bool historyBuilt = false;
    bool earlyDepthBuilt = false;

    // The state the pyramids are read in: by the passes, and with mesh shaders by their far leaves too.
    ImageState pyramidRead = ImageState::ComputeRead;

    // The history's view-projection and camera, and the cull it was kept for: it holds last frame's depth only when kept
    // by the cull before this one (a frame without occlusion, or a resize, leaves it stale).
    Mat4 historyViewProjection;
    Vec3 historyOrigin;
    std::uint64_t culls = 0;
    std::uint64_t historyCull = 0;
    bool historyKept = false;

    // This frame's view-projection and camera, kept by keepDepth.
    Mat4 frameViewProjection;
    Vec3 frameOrigin;
};
