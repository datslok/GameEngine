#pragma once

#include "render/vulkan/owned_handle.h"
#include "render/vulkan/uniform_ring.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_pipeline.h"

#include "volk/volk.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

class VulkanDevice;

/*
* Descriptor sets that live for one frame: each draw's textures (set 2) are written into a fresh set, and all of them are
* thrown away together when the frame's resources are reused, by resetting the pools (far cheaper than freeing sets one
* by one). A pool that runs out is followed by another, kept for later frames.
*/
class FrameDescriptors {
public:
    explicit FrameDescriptors(VkDevice device);

    FrameDescriptors(const FrameDescriptors&) = delete;
    FrameDescriptors& operator=(const FrameDescriptors&) = delete;

    VkDescriptorSet allocate(VkDescriptorSetLayout layout);

    // Only once the GPU has finished the frame that used these sets.
    void reset();

    VkDevice getDevice() const;

private:
    VkDevice device;
    std::vector<OwnedHandle<VkDescriptorPool>> pools;
    std::size_t current = 0;

    void addPool();
};

/*
* A frame's uniform data. Shaders read small blocks of constants (matrices, a material, the lights) from uniform
* buffers; SDL_GPU let each draw "push" new values for a slot, and those stayed in effect until pushed again. This does
* the same: push() copies the data into the frame's buffer at the next aligned offset and remembers it for the slot,
* and bind() binds each stage's set with the latest offsets as dynamic offsets, which is how one descriptor can point
* at a different part of the buffer for every draw. The buffer is upload memory the CPU writes directly.
*
* When a buffer fills up, the next one starts and the slots' latest values are copied into it, so all of a draw's
* blocks are always in one buffer. Buffers are kept and reused every frame; reset() once the GPU has finished with them.
*/
class FrameUniforms {
public:
    FrameUniforms(const VulkanDevice& device, DescriptorLayouts& layouts);
    ~FrameUniforms();

    FrameUniforms(const FrameUniforms&) = delete;
    FrameUniforms& operator=(const FrameUniforms&) = delete;

    // stage is VK_SHADER_STAGE_VERTEX_BIT or VK_SHADER_STAGE_FRAGMENT_BIT; slot is the block's binding in its set.
    void push(VkShaderStageFlagBits stage, std::uint32_t slot, const void* data, std::size_t bytes);

    // Bind sets 1 and 3 for this pipeline. Throws std::logic_error if it reads a slot nothing was pushed to.
    void bind(VkCommandBuffer commands, const VulkanPipeline& pipeline) const;

    // Make this frame's writes visible to the GPU (nothing to do on the usual coherent memory). Before submitting.
    void flush() const;

    void reset();

private:
    // 4 MB holds about 16,000 draws' blocks at 256 bytes each: a heavy shadow frame fits in one or two.
    static constexpr std::size_t bufferBytes = 4 * 1024 * 1024;

    struct Buffer {
        VulkanBuffer memory;
        UniformRing ring;
        OwnedHandle<VkDescriptorPool> pool;
        VkDescriptorSet vertexSet = VK_NULL_HANDLE;
        std::array<VkDescriptorSet, maxStageUniforms + 1> fragmentSets{}; // by block count; 0 unused
    };

    struct Slot {
        std::vector<std::byte> data;
        std::uint32_t offset = 0;
        bool pushed = false;
    };

    const VulkanDevice& device;
    DescriptorLayouts& layouts;
    std::size_t alignment;
    std::vector<std::unique_ptr<Buffer>> buffers;
    std::size_t current = 0;
    std::array<Slot, maxStageUniforms> vertexSlots;
    std::array<Slot, maxStageUniforms> fragmentSlots;

    Buffer& currentBuffer();
    std::unique_ptr<Buffer> makeBuffer();
    std::uint32_t write(const void* data, std::size_t bytes);
    void moveToNextBuffer();
};
