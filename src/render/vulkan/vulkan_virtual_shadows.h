#pragma once

#include "render/gpu/virtual_shadows.h"
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
* The sun's virtual shadow maps' pages on the GPU (render/gpu/virtual_shadows.h; ROADMAP phase 21 stage 4b). Each frame,
* after the depth prepass, four compute passes: clear last frame's flags, mark the pages every pixel's shadow needs,
* resolve each table entry's fate (kept, stale, needing a place, only cached), and allocate places in the pool. Nothing
* is drawn into the pages yet (4c); the counts reach F4, and the main pass reads the table for its debug view.
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
    * pass reads it too). Off, the record says so (no levels, no debug view) and the pages keep their places.
    */
    void prepare(std::uint32_t frameIndex, const VirtualShadowSettings& settings, const Vec3& camera, const Vec3& sunDirection,
                 float casterReach, const Mat4& inverseViewProjection, float width, float height);

    // Whether prepare found the pages on this frame (enabled, with a sun).
    bool runs() const;

    // After the depth prepass (the depth left readable by compute): the four passes, the table left for fragment reads.
    void mark(VkCommandBuffer commands, FrameDescriptors& descriptors, std::uint32_t frameIndex, VulkanImage& depth);

    // Once the frame's fence passed: its counts, once; none when it marked nothing.
    std::optional<VirtualShadowCounts> collect(std::uint32_t frameIndex);

    // For the scene set: this frame's record and the page table (SCENE_VIRTUAL_SHADOW_*_BINDING).
    VkDescriptorBufferInfo frameRecord(std::uint32_t frameIndex) const;
    VkDescriptorBufferInfo pageTable() const;

private:
    struct Readback {
        VulkanBuffer counters;
        bool pending = false;
    };

    const VulkanDevice& device;
    VkSampler nearest;
    std::unique_ptr<ComputePipeline> clearPipeline;
    std::unique_ptr<ComputePipeline> markPipeline;
    std::unique_ptr<ComputePipeline> resolvePipeline;
    std::unique_ptr<ComputePipeline> allocatePipeline;

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
