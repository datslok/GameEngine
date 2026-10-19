#include "render/vulkan/frame_descriptors.h"
#include "render/gpu/uniform_limits.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <cstring>
#include <stdexcept>

namespace {
    // Room for a few hundred draws' textures per pool; a busier frame adds a pool.
    constexpr std::uint32_t setsPerPool = 1024;
    constexpr std::uint32_t texturesPerPool = 8 * setsPerPool;
    constexpr std::uint32_t storageBuffersPerPool = setsPerPool;

    // Only the scene's set holds a plain uniform buffer (its frame data), once per frame.
    constexpr std::uint32_t uniformBuffersPerPool = 16;

    // Only the light probes' update writes images in place: one per atlas, once per frame.
    constexpr std::uint32_t storageImagesPerPool = 16;
}

FrameDescriptors::FrameDescriptors(VkDevice device):
    device(device)
{
    addPool();
}

void FrameDescriptors::addPool() {
    const std::array<VkDescriptorPoolSize, 4> sizes{{
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, texturesPerPool},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, storageBuffersPerPool},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uniformBuffersPerPool},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, storageImagesPerPool},
    }};

    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets = setsPerPool;
    info.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    info.pPoolSizes = sizes.data();

    VkDescriptorPool pool = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorPool(device, &info, nullptr, &pool), "Descriptor pool creation failed");
    pools.emplace_back(device, pool, vkDestroyDescriptorPool);
}

VkDescriptorSet FrameDescriptors::allocate(VkDescriptorSetLayout layout) {
    while (true) {
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = pools[current].get();
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = vkAllocateDescriptorSets(device, &info, &set);

        if (result == VK_SUCCESS) {
            return set;
        }

        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
            vulkanCheck(result, "Descriptor set allocation failed");
        }

        ++current;

        if (current == pools.size()) {
            addPool();
        }
    }
}

VkDevice FrameDescriptors::getDevice() const {
    return device;
}

void FrameDescriptors::reset() {
    for (const OwnedHandle<VkDescriptorPool>& pool : pools) {
        vulkanCheck(vkResetDescriptorPool(device, pool.get(), 0), "Descriptor pool reset failed");
    }

    current = 0;
}

FrameUniforms::FrameUniforms(const VulkanDevice& device, DescriptorLayouts& layouts):
    device(device),
    layouts(layouts),
    alignment(static_cast<std::size_t>(device.getLimits().minUniformBufferOffsetAlignment))
{
    buffers.push_back(makeBuffer());
}

FrameUniforms::~FrameUniforms() = default;

/*
* Each buffer has its own descriptor sets: one per number of blocks a stage reads (1 to 4), every binding pointing at
* the start of the buffer with a 4 KB range. The draw's dynamic offsets then move each binding to its block.
*/
std::unique_ptr<FrameUniforms::Buffer> FrameUniforms::makeBuffer() {
    const VkDevice handle = device.getDevice();
    auto buffer = std::unique_ptr<Buffer>(new Buffer{
        VulkanBuffer{device.getAllocator(), bufferBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, BufferMemory::Upload},
        UniformRing{bufferBytes, alignment, maxUniformBlockBytes},
        {},
        VK_NULL_HANDLE,
        {},
    });

    // Vertex shaders read one block; fragment shaders up to four. One set per count for fragments, plus the vertex set.
    constexpr std::uint32_t fragmentDescriptors = 1 + 2 + 3 + 4;
    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 + fragmentDescriptors};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1 + maxStageUniforms;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &size;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorPool(handle, &poolInfo, nullptr, &pool), "Uniform descriptor pool creation failed");
    buffer->pool = OwnedHandle<VkDescriptorPool>{handle, pool, vkDestroyDescriptorPool};

    auto allocateAndPoint = [&](VkShaderStageFlags stage, std::uint32_t count) {
        const VkDescriptorSetLayout layout = layouts.uniforms(stage, count);

        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = pool;
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        vulkanCheck(vkAllocateDescriptorSets(handle, &info, &set), "Uniform descriptor set allocation failed");

        const VkDescriptorBufferInfo bufferInfo{buffer->memory.get(), 0, maxUniformBlockBytes};
        std::array<VkWriteDescriptorSet, maxStageUniforms> writes{};

        for (std::uint32_t binding = 0; binding < count; ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = set;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorCount = 1;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
            writes[binding].pBufferInfo = &bufferInfo;
        }

        vkUpdateDescriptorSets(handle, count, writes.data(), 0, nullptr);
        return set;
    };

    buffer->vertexSet = allocateAndPoint(VK_SHADER_STAGE_VERTEX_BIT, 1);

    for (std::uint32_t count = 1; count <= maxStageUniforms; ++count) {
        buffer->fragmentSets[count] = allocateAndPoint(VK_SHADER_STAGE_FRAGMENT_BIT, count);
    }

    return buffer;
}

FrameUniforms::Buffer& FrameUniforms::currentBuffer() {
    return *buffers[current];
}

std::uint32_t FrameUniforms::write(const void* data, std::size_t bytes) {
    std::optional<std::size_t> offset = currentBuffer().ring.tryAllocate(bytes);

    if (!offset) {
        moveToNextBuffer();
        offset = currentBuffer().ring.allocate(bytes);
    }

    std::memcpy(static_cast<std::byte*>(currentBuffer().memory.getMapped()) + *offset, data, bytes);
    return static_cast<std::uint32_t>(*offset);
}

// The slots' latest values go along, so a draw never reads blocks from two buffers.
void FrameUniforms::moveToNextBuffer() {
    ++current;

    if (current == buffers.size()) {
        buffers.push_back(makeBuffer());
    }

    currentBuffer().ring.reset();

    for (std::array<Slot, maxStageUniforms>* slots : {&vertexSlots, &fragmentSlots}) {
        for (Slot& slot : *slots) {
            if (slot.pushed) {
                const std::size_t offset = currentBuffer().ring.allocate(slot.data.size());
                std::memcpy(static_cast<std::byte*>(currentBuffer().memory.getMapped()) + offset, slot.data.data(), slot.data.size());
                slot.offset = static_cast<std::uint32_t>(offset);
            }
        }
    }
}

void FrameUniforms::push(VkShaderStageFlagBits stage, std::uint32_t slot, const void* data, std::size_t bytes) {
    if (slot >= maxStageUniforms) {
        throw std::out_of_range("Uniform slot out of range");
    }

    std::array<Slot, maxStageUniforms>& slots = stage == VK_SHADER_STAGE_VERTEX_BIT ? vertexSlots : fragmentSlots;
    const std::uint32_t offset = write(data, bytes);

    Slot& target = slots[slot];
    const auto* first = static_cast<const std::byte*>(data);
    target.data.assign(first, first + bytes);
    target.offset = offset;
    target.pushed = true;
}

void FrameUniforms::bind(VkCommandBuffer commands, const VulkanPipeline& pipeline) const {
    const ShaderResources& resources = pipeline.getResources();
    const Buffer& buffer = *buffers[current];

    auto bindStage = [&](const std::array<Slot, maxStageUniforms>& slots, std::uint32_t count, std::uint32_t set, VkDescriptorSet descriptors) {
        if (count == 0) {
            return;
        }

        std::array<std::uint32_t, maxStageUniforms> offsets{};

        for (std::uint32_t binding = 0; binding < count; ++binding) {
            if (!slots[binding].pushed) {
                throw std::logic_error("A shader reads a uniform block nothing was pushed to");
            }

            offsets[binding] = slots[binding].offset;
        }

        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline.getLayout(), set, 1, &descriptors, count, offsets.data());
    };

    if (resources.vertexUniforms > 1) {
        throw std::logic_error("Vertex shaders read at most one uniform block");
    }

    bindStage(vertexSlots, resources.vertexUniforms, vertexUniformSet, buffer.vertexSet);
    bindStage(fragmentSlots, resources.fragmentUniforms, fragmentUniformSet,
              resources.fragmentUniforms > 0 ? buffer.fragmentSets[resources.fragmentUniforms] : VK_NULL_HANDLE);
}

void FrameUniforms::reset() {
    for (const std::unique_ptr<Buffer>& buffer : buffers) {
        buffer->ring.reset();
    }

    current = 0;

    for (std::array<Slot, maxStageUniforms>* slots : {&vertexSlots, &fragmentSlots}) {
        for (Slot& slot : *slots) {
            slot.pushed = false;
        }
    }
}

void FrameUniforms::flush() const {
    for (std::size_t index = 0; index <= current; ++index) {
        buffers[index]->memory.flush();
    }
}
