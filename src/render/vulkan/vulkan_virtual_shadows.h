#pragma once

#include "render/gpu/cull_records.h"
#include "render/gpu/virtual_shadows.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/owned_handle.h"
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
* The sun's virtual shadow maps' pages on the GPU (render/gpu/virtual_shadows.h; ROADMAP phase 21 stage 4b). Each frame,
* after the depth prepass, four compute passes: clear last frame's flags, mark the pages every pixel's shadow needs,
* resolve each table entry's fate (kept, stale, needing a place, only cached), and allocate places in the pool. Then
* (4c-ii) the casters' surviving clusters are expanded into page pairs, a pair for each drawn page a cluster's sphere
* touches, and the renderer draws each pair's cluster into the pool, a depth image cleared each frame (every page is
* redrawn until 4e caches them). The counts reach F4; the main pass reads the table and the pool for its debug view.
*
* Everything lives on the GPU from frame to frame: the page table, each entry's key, the free list and its top. The CPU
* keeps only the clipmap (where each level is) and each level's generation, bumped when the level drops its pages, so the
* resolving pass knows those entries for stale by their keys. A change of levels, resolution or pool starts it all
* again (every entry empty, every page free).
*/
class VulkanVirtualShadows {
public:
    // nearest: exact texels of the depth, clamped.
    VulkanVirtualShadows(const VulkanDevice& device, VkSampler nearest, std::uint32_t framesInFlight);

    // Between frames only: counts not yet collected are dropped.
    void setFramesInFlight(std::uint32_t count);

    /*
    * Before the frame's scene set is written: the clipmap moved to the camera and this frame's record written (the main
    * pass reads it too). Off, the record says so (no levels, no debug view) and the pages keep their places. biasTexels:
    * the main pass's bias in a level's texels, the cascades' own (ShadowSettings::normalOffsetTexels).
    */
    void prepare(std::uint32_t frameIndex, const VirtualShadowSettings& settings, const Vec3& camera, const Vec3& sunDirection,
                 float casterReach, float biasTexels, const Mat4& inverseViewProjection, float width, float height);

    // Whether prepare found the pages on this frame (enabled, with a sun).
    bool runs() const;

    // The clipmap prepare moved to the camera (none before the first frame that ran).
    const std::optional<VirtualShadowClipmap>& getClipmap() const;

    // After the depth prepass (the depth left readable by compute): the four passes, the table left for fragment reads.
    void mark(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth);

    // Once the frame's fence passed: its counts, once; none when it marked nothing.
    std::optional<VirtualShadowCounts> collect(std::uint32_t frameIndex);

    // For the scene set: this frame's record and the page table (SCENE_VIRTUAL_SHADOW_*_BINDING).
    VkDescriptorBufferInfo frameRecord(std::uint32_t frameIndex) const;
    VkDescriptorBufferInfo pageTable() const;

    // This frame's page pairs' buffer by address, for the frame's uniform block (made by prepare while running).
    VkDeviceAddress pairAddress(std::uint32_t frameIndex) const;

    // After prepare: this frame draws into the pool (its casters culled), so the main pass may read it (the record's options.w).
    void markPoolDrawn(std::uint32_t frameIndex);

    // After prepare: the camera's culling frustum, for the costs' casters seen or not (countCosts, 4c-v).
    void setCameraFrustum(std::uint32_t frameIndex, const GpuFrustum& frustum);

    /*
    * After mark, the casters' clusters culled (VulkanMeshClusterCulling::listPassBindings and its task commands): each
    * style's survivors expanded into page pairs, a workgroup a survivor dispatched by the list's own draw of that style,
    * then each pair list's mesh shader draw written into the pairs' header, which is copied back for F4. Leaves the pairs
    * for the mesh shaders and their draws for indirect reads.
    */
    void expand(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, const std::vector<ComputeBinding>& clusterBindings,
                VkBuffer clusterTasks, std::uint32_t boneStamp);

    /*
    * The pool (VIRTUAL_SHADOW_PAGE_TEXELS a page, virtualShadowPoolColumns pages a row), made by prepare for its settings;
    * none before. Drawn by the depth test, a D32 depth image; by atomics (4c-iv), an R32 float image whose whole-number
    * view the atomics write (virtualShadowAtomicDepth), its own view sampled as depth alike.
    */
    VulkanImage* getPool();

    // After prepare: whether this frame's pool is drawn by atomics (asked, with the GPU's fragment atomics).
    bool drawsByAtomics() const;

    // For set 1's atomics binding: the pool's whole-number view while drawn by atomics, else a 1 x 1 stand-in no shader
    // that runs then reads (the atomics' pipelines are bound only with the pool).
    VkImageView getAtomicPoolView() const;

    // This frame's pairs: each list's mesh shader draw at VIRTUAL_SHADOW_HEADER_TASKS words in.
    VkBuffer getPairs(std::uint32_t frameIndex) const;

    // Once the frame's fence passed: what it drew, once; none when it drew nothing. A list past its room grows the room.
    std::optional<VirtualShadowDrawn> collectDrawn(std::uint32_t frameIndex);

private:
    struct Readback {
        VulkanBuffer counters;
        bool pending = false;

        // The page pairs (4c-ii): GPU memory with the room it was made for, and its header copied back.
        VulkanBuffer pairs;
        std::uint32_t pairRoom = 0;
        VulkanBuffer header;
        bool headerPending = false;
        bool atomics = false; // drawn by atomics, for F4
        bool counted = false; // its costs counted (VirtualShadowSettings::countCosts)
    };

    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> clearPipeline;
    std::unique_ptr<ComputePipeline> markPipeline;
    std::unique_ptr<ComputePipeline> resolvePipeline;
    std::unique_ptr<ComputePipeline> allocatePipeline;
    std::unique_ptr<ComputePipeline> expandPipeline;
    std::unique_ptr<ComputePipeline> tasksPipeline;

    // The pool, made for the pages builtFor has; the pairs' room for frames from now.
    std::unique_ptr<VulkanImage> pool;
    OwnedHandle<VkImageView> atomicView;
    VulkanImage atomicStandIn;
    bool atomics = false;
    std::uint32_t pairRoom = virtualShadowFirstPairRoom;

    // Sized for the most levels, pages and pool any settings ask for, so changing them never reallocates.
    VulkanBuffer table;
    VulkanBuffer keys;
    VulkanBuffer counters;
    VulkanBuffer freePages;
    VulkanBuffer neededEntries;
    VulkanBuffer cachedEntries;
    std::vector<VulkanBuffer> frameRecords;
    std::vector<Readback> readbacks;

    std::optional<VirtualShadowClipmap> clipmap;
    std::array<std::uint32_t, maxVirtualShadowLevels> generations{};
    std::uint32_t frameNumber = 0;
    bool running = false;

    // The settings the GPU's state was built for; others (levels, resolution, pool) start it again.
    std::optional<VirtualShadowSettings> builtFor;
    bool startAgain = true;
};
