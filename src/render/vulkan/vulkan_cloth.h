#pragma once

#include "render/gpu/cloth_gpu.h"
#include "render/gpu/cloth_timing.h"
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
class ClothPassStamps;

/*
* The cloth passes (cloth_predict, cloth_stretch, cloth_bend, cloth_block and cloth_finish.comp, which all bind the same
* fifteen storage buffers, cloth_pass.glsl, so one descriptor set serves them all), the coarse levels' passes
* (cloth_held, cloth_gradient, cloth_coarse_step, cloth_spread, cloth_line_terms, cloth_line_total and
* cloth_line_apply.comp: the same set 0, and the levels' sixteen buffers at set 1, cloth_levels.glsl) and the shape pass
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

    const ComputePipeline& getHeld() const;
    const ComputePipeline& getGradient() const;
    const ComputePipeline& getCoarseStep() const;
    const ComputePipeline& getSpread() const;
    const ComputePipeline& getLineTerms() const;
    const ComputePipeline& getLineTotal() const;
    const ComputePipeline& getLineApply() const;

private:
    std::unique_ptr<ComputePipeline> predict;
    std::unique_ptr<ComputePipeline> stretch;
    std::unique_ptr<ComputePipeline> bend;
    std::unique_ptr<ComputePipeline> block;
    std::unique_ptr<ComputePipeline> finish;
    std::unique_ptr<ComputePipeline> shape;
    std::unique_ptr<ComputePipeline> held;
    std::unique_ptr<ComputePipeline> gradient;
    std::unique_ptr<ComputePipeline> coarseStep;
    std::unique_ptr<ComputePipeline> spread;
    std::unique_ptr<ComputePipeline> lineTerms;
    std::unique_ptr<ComputePipeline> lineTotal;
    std::unique_ptr<ComputePipeline> lineApply;
};

/*
* One cloth living on the GPU: the CPU solver (cloth/cloth.h) ported as it is, XPBD or vertex block descent with its
* coarse levels, as each tick's settings say. Its points, coloured constraints, per-point lists, point colours and
* levels are uploaded once; each tick then needs only its commands (ClothTickCommands: the fabric, the pins' targets, the
* hand, the bodies), written into a small upload buffer, and runs as a string of dispatches with a barrier after each
* (clothDispatchesPerTick): per substep the prediction, the solver's dispatches (XPBD's a colour of stretch or of
* bending each, vertex block descent's a colour of points each: within a colour nothing shares a point, so each runs
* at once, as the CPU's threads do; with levels, the held points, each level's passes coarsest first and a second
* sweep), and the end. A level's line search sums its terms on the GPU and leaves its step length there for the move,
* so a tick never waits for the CPU.
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
/*
* The field bodies' samples on the GPU (DistanceFieldCollider; each tick's GpuClothField records say where each starts):
* every field one after another in one storage buffer, shared by every cloth, since a field is large (the cloth lab's
* mannequin is 24.6 MB) and the same for every cloth that meets it. A field is uploaded once, when it is first wanted:
* the buffer is then built again holding every field so far, after the GPU has finished with the old one (rare: a
* field appears when its body is made). It keeps the fields it holds alive.
*/
class VulkanClothFields {
public:
    explicit VulkanClothFields(const VulkanDevice& device);

    // Hold every one of these fields; if any is new, wait for the GPU and build the buffer again through uploads.
    void hold(ImmediateCommands& uploads, const ClothFields& wanted);

    VkBuffer getSamples() const;

    // Where a held field's samples start in the buffer. Throws std::invalid_argument for a field not held.
    std::uint32_t firstSample(const SignedDistanceField& field) const;

private:
    const VulkanDevice& device;
    ClothFields fields;
    std::vector<std::uint32_t> firsts;
    VulkanBuffer samples;
};

class VulkanCloth {
public:
    VulkanCloth(const VulkanDevice& device, const VulkanClothPipelines& pipelines, ImmediateCommands& uploads, const Cloth& cloth,
                std::uint32_t slots = 1);

    /*
    * Record the ticks, in order, into commands. Throws std::invalid_argument for a slot the cloth does not have, a tick
    * whose pin targets are not one per pin, a hand on a point the cloth does not have, or field bodies without fields
    * holding them. With stamps, a timestamp after each pass, marked with its kind (here and in the other record
    * functions).
    */
    void recordTicks(VkCommandBuffer commands, std::span<const ClothTickCommands> ticks, std::uint32_t slot, ClothPassStamps* stamps = nullptr,
                     const VulkanClothFields* fields = nullptr);

    // Record a copy of the points and velocities into the slot's read-back buffer, after everything recorded before it.
    void recordReadback(VkCommandBuffer commands, std::uint32_t slot, ClothPassStamps* stamps = nullptr);

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
    void recordShape(VkCommandBuffer commands, std::uint32_t meshSlot, float alpha, ClothPassStamps* stamps = nullptr);

    std::uint32_t getPointCount() const;
    std::size_t getPinCount() const;

private:
    struct Slot {
        VulkanBuffer ticks;
        VulkanBuffer pins;
        VulkanBuffer colliders;
        VulkanBuffer fields;
        VulkanBuffer readback;
        VkDescriptorSet set = VK_NULL_HANDLE;

        // The field samples its set binds (VulkanClothFields', or the cloth's empty stand-in).
        VkBuffer fieldSamples = VK_NULL_HANDLE;
    };

    // The coarse levels (render/gpu/cloth_gpu.h's GpuClothLevels) and the work arrays their passes hand on: set 1.
    struct Levels {
        std::vector<GpuClothLevel> records;
        VulkanBuffer gradient;
        VulkanBuffer direction;
        VulkanBuffer coarseSteps;
        VulkanBuffer held;
        VulkanBuffer recordBuffer;
        VulkanBuffer points;
        VulkanBuffer masses;
        VulkanBuffer neighbourStarts;
        VulkanBuffer neighbours;
        VulkanBuffer gatherStarts;
        VulkanBuffer gather;
        VulkanBuffer spreadStarts;
        VulkanBuffer spread;
        VulkanBuffer pointLevels;
        VulkanBuffer linePartials;
        VulkanBuffer lineStep;
        OwnedHandle<VkDescriptorPool> descriptorPool;
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
    std::uint32_t edgeCount = 0;
    std::uint32_t hingeCount = 0;
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

    // Each point's tether slots, read by the end's pass when a tick has the tethers on.
    VulkanBuffer tethers;

    // Bound as the field samples while no ticks have field bodies.
    VulkanBuffer noFieldSamples;

    // The points before the last tick recorded, for drawing between the last two ticks.
    VulkanBuffer tickStart;

    // Where each pin's point is: the start of its next tick's slide.
    std::vector<Vec3> pinPositions;

    OwnedHandle<VkDescriptorPool> descriptorPool;
    std::vector<Slot> slots;
    std::unique_ptr<Shape> shape;

    // None for a cloth without coarse levels.
    std::unique_ptr<Levels> levels;

    Slot& slotAt(std::uint32_t slot);
    const Slot& slotAt(std::uint32_t slot) const;
    // Whether any of the slot's lists had to grow (its set must then be written again).
    bool ensureCapacity(Slot& slot, std::size_t ticks, std::size_t pins, std::size_t colliders, std::size_t fields);
    void writeSet(const Slot& slot);
    void uploadLevels(ImmediateCommands& uploads, const Cloth& cloth);

    // What a substep's passes are recorded into: the commands, the slot's set, the tick and substep, any stamps.
    struct Recording {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        std::uint32_t tick = 0;
        std::uint32_t step = 0;
        ClothPassStamps* stamps = nullptr;
    };

    /*
    * One pass over count items from first, followed by a barrier and any stamp (marked kind); a level's pass (one whose
    * set is set 1) binds the levels' set too and is told its level. groups, if given, is the workgroups to run instead
    * of enough for count.
    */
    void dispatch(const Recording& recording, const ComputePipeline& pipeline, ClothPass kind, std::uint32_t first, std::uint32_t count,
                  std::uint32_t level = 0, std::uint32_t groups = 0) const;
    void recordSweep(const Recording& recording) const;
    void recordLevels(const Recording& recording, const ClothSettings& settings) const;
    void keepTickStart(VkCommandBuffer commands, ClothPassStamps* stamps);
};
