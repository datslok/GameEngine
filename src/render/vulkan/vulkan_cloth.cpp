#include "render/vulkan/vulkan_cloth.h"

#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_mesh.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>

namespace {
    // Must match cloth_pass.glsl's ClothDispatch block.
    struct ClothPushConstants {
        std::uint32_t tick = 0;
        std::uint32_t step = 0;
        std::uint32_t first = 0;
        std::uint32_t count = 0;
    };

    static_assert(sizeof(ClothPushConstants) == 16, "ClothPushConstants must match cloth_pass.glsl's ClothDispatch block");

    // Must match cloth_shape.comp's ClothShape block.
    struct ClothShapePushConstants {
        float alpha = 0.0f;
        std::uint32_t vertexCount = 0;
    };

    static_assert(sizeof(ClothShapePushConstants) == 8, "ClothShapePushConstants must match cloth_shape.comp's ClothShape block");

    // cloth_pass.glsl's bindings, in order: positions, previous positions, velocities, stretch, bending, pin slots, then
    // the slot's ticks, pins and bodies.
    constexpr std::uint32_t clothBindings = 9;

    // cloth_shape.comp's: the points, the points before the last tick, the rest vertices, indices, source vertices, the
    // two adjacency lists (starts and triangles each), and the mesh slot it writes.
    constexpr std::uint32_t shapeBindings = 10;

    // Vulkan has no empty buffers, so a list with nothing in it (a cloth with no hinges, a tick with no bodies) still
    // takes this much.
    constexpr VkDeviceSize smallestBuffer = 16;

    ComputePipelineDescription computePass(const char* shader, std::uint32_t storageBuffers, std::uint32_t pushConstantBytes) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = storageBuffers;
        description.pushConstantBytes = pushConstantBytes;
        return description;
    }

    ComputePipelineDescription clothPass(const char* shader) {
        return computePass(shader, clothBindings, sizeof(ClothPushConstants));
    }

    VkDeviceSize atLeastSmallest(VkDeviceSize bytes) {
        return std::max(bytes, smallestBuffer);
    }

    template <typename Item>
    VkDeviceSize bytesOf(const std::vector<Item>& items) {
        return static_cast<VkDeviceSize>(items.size() * sizeof(Item));
    }

    std::vector<Vec4> asVec4s(const std::vector<Vec3>& values) {
        std::vector<Vec4> packed;
        packed.reserve(values.size());
        for (const Vec3& value : values) {
            packed.push_back(Vec4{value.x, value.y, value.z, 0.0f});
        }
        return packed;
    }

    void memoryBarrier(VkCommandBuffer commands, VkPipelineStageFlags2 sourceStages, VkAccessFlags2 sourceAccess, VkPipelineStageFlags2 laterStages,
                       VkAccessFlags2 laterAccess) {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = sourceStages;
        barrier.srcAccessMask = sourceAccess;
        barrier.dstStageMask = laterStages;
        barrier.dstAccessMask = laterAccess;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    /*
    * Every pass reads and writes the points that the one before it wrote, so each waits for the last to finish and sees
    * its writes. Copies are among what it waits for: the readback and tick-start copies read the points that the next
    * passes overwrite, and the upload's copies write them.
    */
    void clothBarrier(VkCommandBuffer commands) {
        memoryBarrier(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    // Before a copy out of the points: the passes' writes (and any earlier copy into the same buffers) reach it.
    void toCopyBarrier(VkCommandBuffer commands) {
        memoryBarrier(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }

    // Data copied once into GPU memory, through one staging buffer.
    struct Upload {
        const void* data;
        VkDeviceSize bytes;
        VkBuffer destination;
    };

    void uploadAll(const VulkanDevice& device, ImmediateCommands& uploads, std::span<const Upload> copies) {
        VkDeviceSize stagingBytes = 0;
        for (const Upload& copy : copies) {
            stagingBytes += copy.bytes;
        }
        VulkanBuffer staging{device.getAllocator(), atLeastSmallest(stagingBytes), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload};
        auto* destination = static_cast<std::byte*>(staging.getMapped());
        VkDeviceSize offset = 0;
        for (const Upload& copy : copies) {
            if (copy.bytes > 0) {
                std::memcpy(destination + offset, copy.data, static_cast<std::size_t>(copy.bytes));
            }
            offset += copy.bytes;
        }
        staging.flush();

        uploads.run([&](VkCommandBuffer commands) {
            VkDeviceSize from = 0;
            for (const Upload& copy : copies) {
                if (copy.bytes > 0) {
                    const VkBufferCopy region{from, 0, copy.bytes};
                    vkCmdCopyBuffer(commands, staging.get(), copy.destination, 1, &region);
                }
                from += copy.bytes;
            }
            clothBarrier(commands);
        });
    }

    OwnedHandle<VkDescriptorPool> storagePool(const VulkanDevice& device, std::uint32_t sets, std::uint32_t buffersPerSet) {
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, buffersPerSet * sets};
        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.maxSets = sets;
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        vulkanCheck(vkCreateDescriptorPool(device.getDevice(), &info, nullptr, &pool), "Cloth descriptor pool creation failed");
        return OwnedHandle<VkDescriptorPool>{device.getDevice(), pool, vkDestroyDescriptorPool};
    }

    VkDescriptorSet allocateSet(const VulkanDevice& device, VkDescriptorPool pool, VkDescriptorSetLayout layout) {
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = pool;
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &layout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        vulkanCheck(vkAllocateDescriptorSets(device.getDevice(), &allocateInfo, &set), "Cloth descriptor set allocation failed");
        return set;
    }

    // Whole buffers into a set's bindings 0.., in order.
    void writeBuffers(const VulkanDevice& device, VkDescriptorSet set, std::span<const VkBuffer> buffers) {
        std::vector<VkDescriptorBufferInfo> infos(buffers.size());
        std::vector<VkWriteDescriptorSet> writes(buffers.size());
        for (std::size_t binding = 0; binding < buffers.size(); ++binding) {
            infos[binding] = VkDescriptorBufferInfo{buffers[binding], 0, VK_WHOLE_SIZE};
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = set;
            writes[binding].dstBinding = static_cast<std::uint32_t>(binding);
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[binding].pBufferInfo = &infos[binding];
        }
        vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
}

VulkanClothPipelines::VulkanClothPipelines(const VulkanDevice& device):
    predict(std::make_unique<ComputePipeline>(device, clothPass("assets/shaders/cloth_predict.comp.spv"))),
    stretch(std::make_unique<ComputePipeline>(device, clothPass("assets/shaders/cloth_stretch.comp.spv"))),
    bend(std::make_unique<ComputePipeline>(device, clothPass("assets/shaders/cloth_bend.comp.spv"))),
    finish(std::make_unique<ComputePipeline>(device, clothPass("assets/shaders/cloth_finish.comp.spv"))),
    shape(std::make_unique<ComputePipeline>(device, computePass("assets/shaders/cloth_shape.comp.spv", shapeBindings, sizeof(ClothShapePushConstants))))
{
}

const ComputePipeline& VulkanClothPipelines::getPredict() const {
    return *predict;
}

const ComputePipeline& VulkanClothPipelines::getStretch() const {
    return *stretch;
}

const ComputePipeline& VulkanClothPipelines::getBend() const {
    return *bend;
}

const ComputePipeline& VulkanClothPipelines::getFinish() const {
    return *finish;
}

const ComputePipeline& VulkanClothPipelines::getShape() const {
    return *shape;
}

/*
* The points and constraints go through one staging buffer into GPU memory, where every pass reads them many times a
* tick. Each slot's ticks, pins and bodies stay in upload memory the CPU writes directly: they are small and read
* through the GPU's caches.
*/
VulkanCloth::VulkanCloth(const VulkanDevice& device, const VulkanClothPipelines& pipelines, ImmediateCommands& uploads, const Cloth& cloth,
                         std::uint32_t slotCount):
    device(device),
    pipelines(pipelines),
    pointCount(static_cast<std::uint32_t>(cloth.particles.size())),
    stretchColours(cloth.stretchColours),
    bendColours(cloth.bendColours)
{
    if (slotCount == 0) {
        throw std::invalid_argument("A GPU cloth needs at least one slot");
    }

    const std::vector<Vec4> points = packClothPoints(cloth.particles);
    const std::vector<Vec4> previous = asVec4s(cloth.particles.previousPositions);
    const std::vector<Vec4> moving = asVec4s(cloth.particles.velocities);
    const std::vector<GpuStretch> packedStretch = packStretch(cloth.stretch);
    const std::vector<GpuBend> packedBend = packBend(cloth.bend);
    const std::vector<std::uint32_t> packedSlots = clothPinSlots(cloth);
    for (const ClothPin& pin : cloth.pins) {
        pinPositions.push_back(cloth.particles.positions.at(pin.particle));
    }

    const VmaAllocator allocator = device.getAllocator();
    const VkBufferUsageFlags pointUsage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const VkBufferUsageFlags constant = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    positions = VulkanBuffer{allocator, atLeastSmallest(bytesOf(points)), pointUsage, BufferMemory::Gpu};
    previousPositions = VulkanBuffer{allocator, atLeastSmallest(bytesOf(previous)), pointUsage, BufferMemory::Gpu};
    velocities = VulkanBuffer{allocator, atLeastSmallest(bytesOf(moving)), pointUsage, BufferMemory::Gpu};
    tickStart = VulkanBuffer{allocator, atLeastSmallest(bytesOf(points)), pointUsage, BufferMemory::Gpu};
    stretch = VulkanBuffer{allocator, atLeastSmallest(bytesOf(packedStretch)), constant, BufferMemory::Gpu};
    bend = VulkanBuffer{allocator, atLeastSmallest(bytesOf(packedBend)), constant, BufferMemory::Gpu};
    pinSlots = VulkanBuffer{allocator, atLeastSmallest(bytesOf(packedSlots)), constant, BufferMemory::Gpu};

    // Until a tick runs, the cloth is drawn where it is.
    const std::array<Upload, 7> copies{
        Upload{points.data(), bytesOf(points), positions.get()},
        Upload{previous.data(), bytesOf(previous), previousPositions.get()},
        Upload{moving.data(), bytesOf(moving), velocities.get()},
        Upload{points.data(), bytesOf(points), tickStart.get()},
        Upload{packedStretch.data(), bytesOf(packedStretch), stretch.get()},
        Upload{packedBend.data(), bytesOf(packedBend), bend.get()},
        Upload{packedSlots.data(), bytesOf(packedSlots), pinSlots.get()},
    };
    uploadAll(device, uploads, copies);

    descriptorPool = storagePool(device, slotCount, clothBindings);

    // Every pass's set layout is the same nine storage buffers, so a set made for one is compatible with them all.
    const VkDescriptorSetLayout layout = pipelines.getPredict().getResourceLayout();
    slots.resize(slotCount);
    for (Slot& slot : slots) {
        slot.set = allocateSet(device, descriptorPool.get(), layout);
        slot.readback = VulkanBuffer{allocator, atLeastSmallest(2 * bytesOf(points)), VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback};
        ensureCapacity(slot, 1, 1, 1);
    }
}

VulkanCloth::Slot& VulkanCloth::slotAt(std::uint32_t slot) {
    if (slot >= slots.size()) {
        throw std::invalid_argument("The GPU cloth has no such slot");
    }
    return slots[slot];
}

const VulkanCloth::Slot& VulkanCloth::slotAt(std::uint32_t slot) const {
    if (slot >= slots.size()) {
        throw std::invalid_argument("The GPU cloth has no such slot");
    }
    return slots[slot];
}

// Each list grows to the next power of two that holds it, so a cloth settles on its buffers after a few frames.
void VulkanCloth::ensureCapacity(Slot& slot, std::size_t ticks, std::size_t pins, std::size_t colliders) {
    const VmaAllocator allocator = device.getAllocator();
    bool grew = false;
    const auto grow = [&](VulkanBuffer& buffer, std::size_t count, std::size_t itemBytes) {
        const VkDeviceSize needed = atLeastSmallest(static_cast<VkDeviceSize>(count * itemBytes));
        if (buffer.get() != VK_NULL_HANDLE && buffer.getSize() >= needed) {
            return;
        }
        VkDeviceSize capacity = smallestBuffer;
        while (capacity < needed) {
            capacity *= 2;
        }
        buffer = VulkanBuffer{allocator, capacity, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, BufferMemory::Upload};
        grew = true;
    };

    grow(slot.ticks, ticks, sizeof(GpuClothTick));
    grow(slot.pins, pins, sizeof(GpuClothPin));
    grow(slot.colliders, colliders, sizeof(GpuCollider));
    if (grew) {
        writeSet(slot);
    }
}

void VulkanCloth::writeSet(const Slot& slot) {
    const std::array<VkBuffer, clothBindings> buffers{positions.get(), previousPositions.get(), velocities.get(), stretch.get(), bend.get(),
                                                      pinSlots.get(), slot.ticks.get(), slot.pins.get(), slot.colliders.get()};
    writeBuffers(device, slot.set, buffers);
}

void VulkanCloth::dispatch(VkCommandBuffer commands, const ComputePipeline& pipeline, VkDescriptorSet set, std::uint32_t tick, std::uint32_t step,
                           std::uint32_t first, std::uint32_t count) const {
    if (count == 0) {
        return;
    }
    const ClothPushConstants constants{tick, step, first, count};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(commands, workgroupCount(count, clothGroupSize), 1, 1);
    clothBarrier(commands);
}

// The points as they are, before the next tick changes them; the shape pass blends from these.
void VulkanCloth::keepTickStart(VkCommandBuffer commands) {
    toCopyBarrier(commands);
    const VkBufferCopy region{0, 0, static_cast<VkDeviceSize>(pointCount) * sizeof(Vec4)};
    if (region.size > 0) {
        vkCmdCopyBuffer(commands, positions.get(), tickStart.get(), 1, &region);
    }
    clothBarrier(commands);
}

/*
* The ticks' records, pins and bodies are written for the whole string first, each tick's pins sliding from where the
* last one left them. Then, per tick and substep, the CPU solver's order: predict and the pins, stretch colour by
* colour, bending colour by colour, then the hand, bodies and velocities. The first barrier waits for whatever wrote
* the points before (an earlier string's passes, its readback copy, the upload). Before the last tick the points are
* kept aside, so the frame can be drawn between the last two ticks.
*/
void VulkanCloth::recordTicks(VkCommandBuffer commands, std::span<const ClothTickCommands> ticks, std::uint32_t slotIndex) {
    Slot& slot = slotAt(slotIndex);
    std::size_t pinCount = 0;
    std::size_t colliderCount = 0;
    for (const ClothTickCommands& tick : ticks) {
        if (tick.pinTargets.size() != pinPositions.size()) {
            throw std::invalid_argument("A GPU cloth's tick needs one target per pin");
        }
        if (tick.grab && tick.grab->particle >= pointCount) {
            throw std::invalid_argument("A GPU cloth's hand holds a point the cloth does not have");
        }
        pinCount += tick.pinTargets.size();
        colliderCount += tick.colliders.size();
    }
    if (ticks.empty()) {
        return;
    }
    ensureCapacity(slot, ticks.size(), pinCount, colliderCount);

    auto* tickRecords = static_cast<GpuClothTick*>(slot.ticks.getMapped());
    auto* pinRecords = static_cast<GpuClothPin*>(slot.pins.getMapped());
    auto* colliderRecords = static_cast<GpuCollider*>(slot.colliders.getMapped());
    std::uint32_t pinFirst = 0;
    std::uint32_t colliderFirst = 0;
    for (std::size_t index = 0; index < ticks.size(); ++index) {
        const ClothTickCommands& tick = ticks[index];
        tickRecords[index] = packClothTick(tick, pinFirst, colliderFirst);
        for (std::size_t pin = 0; pin < tick.pinTargets.size(); ++pin) {
            const Vec3& start = pinPositions[pin];
            const Vec3& target = tick.pinTargets[pin];
            pinRecords[pinFirst + pin] = GpuClothPin{Vec4{start.x, start.y, start.z, 1.0f}, Vec4{target.x, target.y, target.z, 1.0f}};
            pinPositions[pin] = target;
        }
        for (std::size_t body = 0; body < tick.colliders.size(); ++body) {
            colliderRecords[colliderFirst + body] = packCollider(tick.colliders[body]);
        }
        pinFirst += static_cast<std::uint32_t>(tick.pinTargets.size());
        colliderFirst += static_cast<std::uint32_t>(tick.colliders.size());
    }
    slot.ticks.flush();
    slot.pins.flush();
    slot.colliders.flush();

    device.beginLabel(commands, "Cloth");
    clothBarrier(commands);
    for (std::size_t index = 0; index < ticks.size(); ++index) {
        if (index + 1 == ticks.size()) {
            keepTickStart(commands);
        }
        const std::uint32_t tick = static_cast<std::uint32_t>(index);
        const ClothSettings& settings = ticks[index].settings;
        for (std::uint32_t step = 0; step < static_cast<std::uint32_t>(settings.substeps); ++step) {
            dispatch(commands, pipelines.getPredict(), slot.set, tick, step, 0, pointCount);
            for (std::size_t colour = 0; colour + 1 < stretchColours.size(); ++colour) {
                dispatch(commands, pipelines.getStretch(), slot.set, tick, step, stretchColours[colour], stretchColours[colour + 1] - stretchColours[colour]);
            }
            if (settings.bendingStiffness > 0.0f) {
                for (std::size_t colour = 0; colour + 1 < bendColours.size(); ++colour) {
                    dispatch(commands, pipelines.getBend(), slot.set, tick, step, bendColours[colour], bendColours[colour + 1] - bendColours[colour]);
                }
            }
            dispatch(commands, pipelines.getFinish(), slot.set, tick, step, 0, pointCount);
        }
    }
    device.endLabel(commands);
}

// The points, then the velocities, one after the other; the copy's writes reach the host once the fence passes.
void VulkanCloth::recordReadback(VkCommandBuffer commands, std::uint32_t slotIndex) {
    Slot& slot = slotAt(slotIndex);
    toCopyBarrier(commands);

    const VkDeviceSize bytes = static_cast<VkDeviceSize>(pointCount) * sizeof(Vec4);
    if (bytes > 0) {
        const VkBufferCopy pointRegion{0, 0, bytes};
        vkCmdCopyBuffer(commands, positions.get(), slot.readback.get(), 1, &pointRegion);
        const VkBufferCopy velocityRegion{0, bytes, bytes};
        vkCmdCopyBuffer(commands, velocities.get(), slot.readback.get(), 1, &velocityRegion);
    }

    memoryBarrier(commands, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT,
                  VK_ACCESS_2_HOST_READ_BIT);
}

namespace {
    std::vector<Vec3> readVec4s(const VulkanBuffer& buffer, std::uint32_t first, std::uint32_t count) {
        buffer.invalidate();
        const auto* values = static_cast<const Vec4*>(buffer.getMapped()) + first;
        std::vector<Vec3> read;
        read.reserve(count);
        for (std::uint32_t index = 0; index < count; ++index) {
            read.push_back(Vec3{values[index].x, values[index].y, values[index].z});
        }
        return read;
    }
}

std::vector<Vec3> VulkanCloth::readPositions(std::uint32_t slot) const {
    return readVec4s(slotAt(slot).readback, 0, pointCount);
}

std::vector<Vec3> VulkanCloth::readVelocities(std::uint32_t slot) const {
    return readVec4s(slotAt(slot).readback, pointCount, pointCount);
}

/*
* The rest vertices (for their UVs and tangent sides), the triangles and which triangles each vertex adds up go into
* GPU memory once; a set per mesh slot points the pass at the slot it writes, since each frame writes the next.
*/
void VulkanCloth::attachShape(ImmediateCommands& uploads, const DeformingTopology& topology, const VulkanMesh& mesh) {
    if (topology.sourceVertexCount != pointCount || mesh.getVertexCount() != topology.rest.vertices.size()) {
        throw std::invalid_argument("A cloth's shape needs one Mesh vertex per point and the mesh made from that topology");
    }

    const DeformingAdjacency adjacency = buildDeformingAdjacency(topology);
    auto attached = std::make_unique<Shape>();
    attached->vertexCount = static_cast<std::uint32_t>(topology.rest.vertices.size());

    const VmaAllocator allocator = device.getAllocator();
    const VkBufferUsageFlags constant = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    const auto made = [&](VkDeviceSize bytes) {
        return VulkanBuffer{allocator, atLeastSmallest(bytes), constant, BufferMemory::Gpu};
    };
    attached->restVertices = made(bytesOf(topology.rest.vertices));
    attached->indices = made(bytesOf(topology.rest.indices));
    attached->sourceVertex = made(bytesOf(topology.sourceVertex));
    attached->sourceStarts = made(bytesOf(adjacency.sourceStarts));
    attached->sourceTriangles = made(bytesOf(adjacency.sourceTriangles));
    attached->vertexStarts = made(bytesOf(adjacency.vertexStarts));
    attached->vertexTriangles = made(bytesOf(adjacency.vertexTriangles));

    const std::array<Upload, 7> copies{
        Upload{topology.rest.vertices.data(), bytesOf(topology.rest.vertices), attached->restVertices.get()},
        Upload{topology.rest.indices.data(), bytesOf(topology.rest.indices), attached->indices.get()},
        Upload{topology.sourceVertex.data(), bytesOf(topology.sourceVertex), attached->sourceVertex.get()},
        Upload{adjacency.sourceStarts.data(), bytesOf(adjacency.sourceStarts), attached->sourceStarts.get()},
        Upload{adjacency.sourceTriangles.data(), bytesOf(adjacency.sourceTriangles), attached->sourceTriangles.get()},
        Upload{adjacency.vertexStarts.data(), bytesOf(adjacency.vertexStarts), attached->vertexStarts.get()},
        Upload{adjacency.vertexTriangles.data(), bytesOf(adjacency.vertexTriangles), attached->vertexTriangles.get()},
    };
    uploadAll(device, uploads, copies);

    attached->descriptorPool = storagePool(device, deformingSlotCount, shapeBindings);
    for (std::uint32_t meshSlot = 0; meshSlot < deformingSlotCount; ++meshSlot) {
        attached->sets[meshSlot] = allocateSet(device, attached->descriptorPool.get(), pipelines.getShape().getResourceLayout());
        const std::array<VkBuffer, shapeBindings> buffers{positions.get(),
                                                          tickStart.get(),
                                                          attached->restVertices.get(),
                                                          attached->indices.get(),
                                                          attached->sourceVertex.get(),
                                                          attached->sourceStarts.get(),
                                                          attached->sourceTriangles.get(),
                                                          attached->vertexStarts.get(),
                                                          attached->vertexTriangles.get(),
                                                          mesh.getSlotBuffer(meshSlot)};
        writeBuffers(device, attached->sets[meshSlot], buffers);
    }
    shape = std::move(attached);
}

bool VulkanCloth::hasShape() const {
    return shape != nullptr;
}

/*
* The points' last writes reach the pass; its writes reach everything that reads a mesh's vertices: drawing (and the
* velocity pass's last-frame stream), building its acceleration structure, and shaders reading a ray hit's corners.
* The slot written was last read frames ago, which the slot ring (render/gpu/deforming_slots.h) has already waited for.
*/
void VulkanCloth::recordShape(VkCommandBuffer commands, std::uint32_t meshSlot, float alpha) {
    if (!shape) {
        throw std::logic_error("This GPU cloth has no shape attached");
    }
    if (meshSlot >= deformingSlotCount) {
        throw std::invalid_argument("A deforming mesh has no such slot");
    }

    device.beginLabel(commands, "Cloth shape");
    clothBarrier(commands);
    const ComputePipeline& pipeline = pipelines.getShape();
    const ClothShapePushConstants constants{alpha, shape->vertexCount};
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, 1, &shape->sets[meshSlot], 0, nullptr);
    vkCmdPushConstants(commands, pipeline.getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants), &constants);
    vkCmdDispatch(commands, workgroupCount(shape->vertexCount, clothGroupSize), 1, 1);

    VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    VkAccessFlags2 reads = VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_READ_BIT;
    if (device.supportsRayQueries()) {
        readers |= VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
    }
    memoryBarrier(commands, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, readers, reads);
    device.endLabel(commands);
}

std::uint32_t VulkanCloth::getPointCount() const {
    return pointCount;
}

std::size_t VulkanCloth::getPinCount() const {
    return pinPositions.size();
}
