#pragma once

#include "render/gpu/cloth_gpu.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"

#include "volk/volk.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

class VulkanDevice;
class ImmediateCommands;

/*
* The cloth passes (cloth_predict, cloth_stretch, cloth_bend and cloth_finish.comp), built once and shared by every
* cloth. They all bind the same nine storage buffers (cloth_pass.glsl), so one descriptor set serves them all.
*/
class VulkanClothPipelines {
public:
    explicit VulkanClothPipelines(const VulkanDevice& device);

    const ComputePipeline& getPredict() const;
    const ComputePipeline& getStretch() const;
    const ComputePipeline& getBend() const;
    const ComputePipeline& getFinish() const;

private:
    std::unique_ptr<ComputePipeline> predict;
    std::unique_ptr<ComputePipeline> stretch;
    std::unique_ptr<ComputePipeline> bend;
    std::unique_ptr<ComputePipeline> finish;
};

/*
* One cloth living on the GPU: the CPU solver's XPBD (cloth/cloth.h) ported as it is. Its points and coloured
* constraints are uploaded once; each tick then needs only its commands (ClothTickCommands: the fabric, the pins'
* targets, the hand, the bodies), written into a small upload buffer, and runs as a string of dispatches with a barrier
* after each (clothDispatchesPerTick): per substep the prediction, a dispatch per colour of stretch and of bending
* (within a colour no two constraints share a point, so each runs at once, as the CPU's threads do), and the end.
*
* The result is the CPU solver's to within rounding, not to the bit (the GPU may fuse a multiply and an add, and
* rounds division and square roots its own way), and repeats exactly on the same GPU and driver: nothing in a pass
* depends on the order its invocations run in.
*
* Slots let frames in flight each keep their own ticks and read-back copy: a slot must not be recorded into while
* commands that used it may still run, and takes one recordTicks per submission (a second would overwrite the first's
* ticks before they run). The pins are the cloth's when it was uploaded.
*/
class VulkanCloth {
public:
    VulkanCloth(const VulkanDevice& device, const VulkanClothPipelines& pipelines, ImmediateCommands& uploads, const Cloth& cloth,
                std::uint32_t slots = 1);

    /*
    * Record the ticks, in order, into commands. Throws std::invalid_argument for a slot the cloth does not have, a tick
    * whose pin targets are not one per pin, or a hand on a point the cloth does not have.
    */
    void recordTicks(VkCommandBuffer commands, std::span<const ClothTickCommands> ticks, std::uint32_t slot);

    // Record a copy of the points into the slot's read-back buffer, after everything recorded before it.
    void recordReadback(VkCommandBuffer commands, std::uint32_t slot);

    // Once the commands with the slot's read-back have finished: the points' positions they copied.
    std::vector<Vec3> readPositions(std::uint32_t slot) const;

    std::uint32_t getPointCount() const;

private:
    struct Slot {
        VulkanBuffer ticks;
        VulkanBuffer pins;
        VulkanBuffer colliders;
        VulkanBuffer readback;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    const VulkanDevice& device;
    const VulkanClothPipelines& pipelines;
    std::uint32_t pointCount = 0;
    std::vector<std::uint32_t> stretchColours;
    std::vector<std::uint32_t> bendColours;

    VulkanBuffer positions;
    VulkanBuffer previousPositions;
    VulkanBuffer velocities;
    VulkanBuffer stretch;
    VulkanBuffer bend;
    VulkanBuffer pinSlots;

    // Where each pin's point is: the start of its next tick's slide.
    std::vector<Vec3> pinPositions;

    OwnedHandle<VkDescriptorPool> descriptorPool;
    std::vector<Slot> slots;

    Slot& slotAt(std::uint32_t slot);
    void ensureCapacity(Slot& slot, std::size_t ticks, std::size_t pins, std::size_t colliders);
    void writeSet(const Slot& slot);
    void dispatch(VkCommandBuffer commands, const ComputePipeline& pipeline, VkDescriptorSet set, std::uint32_t tick, std::uint32_t step,
                  std::uint32_t first, std::uint32_t count) const;
};
