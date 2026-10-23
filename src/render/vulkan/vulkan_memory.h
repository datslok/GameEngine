#pragma once

#include "render/vulkan/image_state.h"
#include "render/vulkan/owned_handle.h"

#include "vma/vk_mem_alloc.h"
#include "volk/volk.h"

#include <cstdint>

class VulkanDevice;

/*
* Where a buffer lives. GPU memory is fastest for the GPU but the CPU cannot see it, so data gets there by copying.
* Upload memory is written by the CPU (in order, never read back) and read by the GPU: on GPUs with resizable BAR it
* is GPU memory the CPU can write over the bus, otherwise system memory the GPU reads across it. Read-back memory is
* written by the GPU and read by the CPU.
*/
enum class BufferMemory {
    Gpu,
    Upload,
    Readback
};

// A buffer and its memory, freed together. Upload and read-back buffers stay mapped, so the CPU reaches them directly.
class VulkanBuffer {
public:
    VulkanBuffer() = default;
    VulkanBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, BufferMemory memory);
    ~VulkanBuffer();

    VulkanBuffer(const VulkanBuffer&) = delete;
    VulkanBuffer& operator=(const VulkanBuffer&) = delete;
    VulkanBuffer(VulkanBuffer&& other) noexcept;
    VulkanBuffer& operator=(VulkanBuffer&& other) noexcept;

    VkBuffer get() const;
    VkDeviceSize getSize() const;

    // Where the buffer starts in the GPU's address space, for shaders that follow it as a pointer and for building
    // acceleration structures. The buffer must have been made with VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT.
    VkDeviceAddress getDeviceAddress() const;

    // The CPU's view of an upload or read-back buffer (null for GPU memory).
    void* getMapped() const;

    // After the GPU wrote a read-back buffer: make its writes visible to the CPU, where memory is not kept coherent.
    void invalidate() const;

    // After the CPU wrote an upload buffer: make its writes visible to the GPU, where memory is not kept coherent.
    void flush() const;

private:
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
    void* mapped = nullptr;

    void release() noexcept;
};

// What an image is: its format, size, mip levels and the uses it must support.
struct ImageDescription {
    VkFormat format = VK_FORMAT_UNDEFINED;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t levels = 1;

    // Array layers; a cube has six (+X, -X, +Y, -Y, +Z, -Z) and its view samples them as one cube.
    std::uint32_t layers = 1;
    bool cube = false;
    VkImageUsageFlags usage = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    const char* name = nullptr; // for RenderDoc, Nsight and validation messages (a string literal: it is kept)
};

/*
* A 2D image in GPU memory with a view of all its levels (what descriptors and render passes name), and the state it
* was last left in, so the next use knows which barrier to record. Every level is kept in the same state, except
* during mipmap generation, which tracks its levels itself and sets the state when done.
*/
class VulkanImage {
public:
    VulkanImage() = default;
    VulkanImage(const VulkanDevice& device, const ImageDescription& description);
    ~VulkanImage();

    VulkanImage(const VulkanImage&) = delete;
    VulkanImage& operator=(const VulkanImage&) = delete;
    VulkanImage(VulkanImage&& other) noexcept;
    VulkanImage& operator=(VulkanImage&& other) noexcept;

    VkImage get() const;
    VkImageView getView() const;
    const ImageDescription& getDescription() const;
    ImageState getState() const;

    // Record the barrier from the image's current state to this one (none when only reading on), and remember it.
    void transition(VkCommandBuffer commands, ImageState to, ImageContents contents = ImageContents::Keep);

    // For code that records its own barriers (mipmap generation).
    void setState(ImageState state);

    explicit operator bool() const;

private:
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    OwnedHandle<VkImageView> view;
    ImageDescription description;
    ImageState state = ImageState::Undefined;

    void release() noexcept;
};

// One image barrier, recorded on its own.
void recordImageBarrier(VkCommandBuffer commands, const VkImageMemoryBarrier2& barrier);

/*
* Every smaller level of an image made from level 0 (in state levelZero), each blitted from the one above with linear
* filtering (in every layer at once), and every level left in finalState. Levels 1 onwards are overwritten whatever
* they held.
*/
void generateMipmaps(VkCommandBuffer commands, VulkanImage& image, ImageState levelZero, ImageState finalState);

/*
* Between two levels of a pyramid built by compute one level at a time, the image kept in the general layout: the
* level just written made visible to the next dispatch, which reads it as a storage image or samples it. Only that
* level, in every layer.
*/
void computeLevelWritten(VkCommandBuffer commands, const VulkanImage& image, std::uint32_t level);

// For images the renderer does not own (the swapchain's): record the barrier and update the state it keeps for them.
void transitionImage(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, ImageState& state, ImageState to,
                     ImageContents contents = ImageContents::Keep);
