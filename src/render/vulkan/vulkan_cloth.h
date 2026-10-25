#pragma once

#include "render/gpu/cloth_gpu.h"
#include "render/gpu/deforming_slots.h"
#include "render/vulkan/owned_handle.h"
#include "render/vulkan/vulkan_compute.h"
#include "render/vulkan/vulkan_memory.h"
#include "scene/deforming_mesh.h"

#include "volk/volk.h"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

class VulkanDevice;
class VulkanMesh;
class ImmediateCommands;

/*
* The cloth passes (cloth_predict, cloth_stretch, cloth_bend, cloth_block and cloth_finish.comp, which all bind the same
* fifteen storage buffers, cloth_pass.glsl, so one descriptor set serves them all) and the shape pass
* (cloth_shape.comp), built once and shared by every cloth.
*/
class VulkanClothPipelines {
public:
    explicit VulkanClothPipelines(const VulkanDevice& device);

    const ComputePipeline& getPredict() const;
    const ComputePipeline& getStretch() const;
    const ComputePipeline& getBend() const;
    const ComputePipeline& getBlock() const;
    const ComputePipeline& getFinish() const;
    const ComputePipeline& getShape() const;

private:
    std::unique_ptr<ComputePipeline> predict;
    std::unique_ptr<ComputePipeline> stretch;
    std::unique_ptr<ComputePipeline> bend;
    std::unique_ptr<ComputePipeline> block;
    std::unique_ptr<ComputePipeline> finish;
    std::unique_ptr<ComputePipeline> shape;
};

/*
* One cloth living on the GPU: the CPU solver (cloth/cloth.h) ported as it is, XPBD or vertex block descent, as each
* tick's settings say (the coarse levels not yet). Its points, coloured constraints, per-point lists and point colours
* are uploaded once; each tick then needs only its commands (ClothTickCommands: the fabric, the pins' targets, the
* hand, the bodies), written into a small upload buffer, and runs as a string of dispatches with a barrier after each
* (clothDispatchesPerTick): per substep the prediction, the solver's dispatches (XPBD's a colour of stretch or of
* bending each, vertex block descent's a colour of points each: within a colour nothing shares a point, so each runs
* at once, as the CPU's threads do), and the end.
*
* The result is the CPU solver's to within rounding, not to the bit (the GPU may fuse a multiply and an add, and
* rounds division and square roots its own way), and repeats exactly on the same GPU and driver: nothing in a pass
* depends on the order its invocations run in.
*
* Drawn straight from its points: with a deforming mesh attached (its topology), recordShape writes the mesh's
* vertices into one of its slots, blended between the points before the last tick recorded (kept aside by recordTicks)
* and after it.
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

    // Record a copy of the points and velocities into the slot's read-back buffer, after everything recorded before it.
    void recordReadback(VkCommandBuffer commands, std::uint32_t slot);

    // Once the commands with the slot's read-back have finished: the points' positions and velocities they copied.
    std::vector<Vec3> readPositions(std::uint32_t slot) const;
    std::vector<Vec3> readVelocities(std::uint32_t slot) const;

    /*
    * Shape this deforming mesh from the cloth's points (the mesh must have been uploaded as deforming from the topology's
    * rest shape, with one Mesh vertex per point). Throws std::invalid_argument for a topology of another size.
    */
    void attachShape(ImmediateCommands& uploads, const DeformingTopology& topology, const VulkanMesh& mesh);
    bool hasShape() const;

    /*
    * Record the shape pass into the mesh's slot meshSlot (VulkanMesh::advanceGpuShape), alpha of the way from the points
    * before the last tick to after it, followed by a barrier making the vertices readable by everything that draws or
    * traces them.
    */
    void recordShape(VkCommandBuffer commands, std::uint32_t meshSlot, float alpha);

    std::uint32_t getPointCount() const;
    std::size_t getPinCount() const;

private:
    struct Slot {
        VulkanBuffer ticks;
        VulkanBuffer pins;
        VulkanBuffer colliders;
        VulkanBuffer readback;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    // What the shape pass reads besides the points, and a set per mesh slot it may write.
    struct Shape {
        VulkanBuffer restVertices;
        VulkanBuffer indices;
        VulkanBuffer sourceVertex;
        VulkanBuffer sourceStarts;
        VulkanBuffer sourceTriangles;
        VulkanBuffer vertexStarts;
        VulkanBuffer vertexTriangles;
        std::uint32_t vertexCount = 0;
        OwnedHandle<VkDescriptorPool> descriptorPool;
        std::array<VkDescriptorSet, deformingSlotCount> sets{};
    };

    const VulkanDevice& device;
    const VulkanClothPipelines& pipelines;
    std::uint32_t pointCount = 0;
    std::vector<std::uint32_t> stretchColours;
    std::vector<std::uint32_t> bendColours;
    std::vector<std::uint32_t> vertexColours;

    VulkanBuffer positions;
    VulkanBuffer previousPositions;
    VulkanBuffer velocities;
    VulkanBuffer stretch;
    VulkanBuffer bend;
    VulkanBuffer pinSlots;

    // Vertex block descent's: the substep's prediction, each point's constraints, the points in colour order.
    VulkanBuffer predicted;
    VulkanBuffer edgeStarts;
    VulkanBuffer pointEdges;
    VulkanBuffer hingeStarts;
    VulkanBuffer pointHinges;
    VulkanBuffer vertexOrder;

    // The points before the last tick recorded, for drawing between the last two ticks.
    VulkanBuffer tickStart;

    // Where each pin's point is: the start of its next tick's slide.
    std::vector<Vec3> pinPositions;

    OwnedHandle<VkDescriptorPool> descriptorPool;
    std::vector<Slot> slots;
    std::unique_ptr<Shape> shape;

    Slot& slotAt(std::uint32_t slot);
    const Slot& slotAt(std::uint32_t slot) const;
    void ensureCapacity(Slot& slot, std::size_t ticks, std::size_t pins, std::size_t colliders);
    void writeSet(const Slot& slot);
    void dispatch(VkCommandBuffer commands, const ComputePipeline& pipeline, VkDescriptorSet set, std::uint32_t tick, std::uint32_t step,
                  std::uint32_t first, std::uint32_t count) const;
    void keepTickStart(VkCommandBuffer commands);
};
