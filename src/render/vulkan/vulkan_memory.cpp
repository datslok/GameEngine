#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <stdexcept>
#include <utility>

/*
* VMA chooses the memory type from what the buffer is for. Upload buffers ask for sequential CPU writes (write-combined
* memory: fast to write in order, very slow to read) and read-back buffers for random CPU access (cached memory).
*/
VulkanBuffer::VulkanBuffer(VmaAllocator allocator, VkDeviceSize size, VkBufferUsageFlags usage, BufferMemory memory):
    allocator(allocator),
    size(size)
{
    if (size == 0) {
        throw std::invalid_argument("A buffer needs at least one byte");
    }

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO;

    switch (memory) {
    case BufferMemory::Gpu:
        allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        break;
    case BufferMemory::Upload:
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    case BufferMemory::Readback:
        allocationInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        break;
    }

    VmaAllocationInfo allocated{};
    vulkanCheck(vmaCreateBuffer(allocator, &bufferInfo, &allocationInfo, &buffer, &allocation, &allocated), "Buffer creation failed");
    mapped = allocated.pMappedData;
}

VulkanBuffer::~VulkanBuffer() {
    release();
}

VulkanBuffer::VulkanBuffer(VulkanBuffer&& other) noexcept:
    allocator(other.allocator),
    buffer(std::exchange(other.buffer, VK_NULL_HANDLE)),
    allocation(std::exchange(other.allocation, VK_NULL_HANDLE)),
    size(std::exchange(other.size, 0)),
    mapped(std::exchange(other.mapped, nullptr))
{
}

VulkanBuffer& VulkanBuffer::operator=(VulkanBuffer&& other) noexcept {
    if (this != &other) {
        release();
        allocator = other.allocator;
        buffer = std::exchange(other.buffer, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        size = std::exchange(other.size, 0);
        mapped = std::exchange(other.mapped, nullptr);
    }

    return *this;
}

void VulkanBuffer::release() noexcept {
    if (buffer != VK_NULL_HANDLE) {
        vmaDestroyBuffer(allocator, buffer, allocation);
        buffer = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
        mapped = nullptr;
        size = 0;
    }
}

VkBuffer VulkanBuffer::get() const {
    return buffer;
}

VkDeviceSize VulkanBuffer::getSize() const {
    return size;
}

void* VulkanBuffer::getMapped() const {
    return mapped;
}

void VulkanBuffer::invalidate() const {
    vulkanCheck(vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE), "Read-back invalidation failed");
}

void VulkanBuffer::flush() const {
    vulkanCheck(vmaFlushAllocation(allocator, allocation, 0, VK_WHOLE_SIZE), "Upload flush failed");
}

/*
* Optimal tiling lets the GPU arrange texels however suits its caches (in tiles rather than rows), which is why images
* are only reached through copies and views, never by the CPU directly.
*/
VulkanImage::VulkanImage(const VulkanDevice& device, const ImageDescription& description):
    allocator(device.getAllocator()),
    description(description)
{
    if (description.width == 0 || description.height == 0 || description.levels == 0) {
        throw std::invalid_argument("Image dimensions and level count must be positive");
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = description.format;
    imageInfo.extent = {description.width, description.height, 1};
    imageInfo.mipLevels = description.levels;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = description.usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo allocationInfo{};
    allocationInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;

    vulkanCheck(vmaCreateImage(allocator, &imageInfo, &allocationInfo, &image, &allocation, nullptr), "Image creation failed");

    try {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = description.format;
        viewInfo.subresourceRange.aspectMask = description.aspect;
        viewInfo.subresourceRange.levelCount = description.levels;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView created = VK_NULL_HANDLE;
        vulkanCheck(vkCreateImageView(device.getDevice(), &viewInfo, nullptr, &created), "Image view creation failed");
        view = OwnedHandle<VkImageView>{device.getDevice(), created, vkDestroyImageView};
    }
    catch (...) {
        release();
        throw;
    }
}

VulkanImage::~VulkanImage() {
    release();
}

VulkanImage::VulkanImage(VulkanImage&& other) noexcept:
    allocator(other.allocator),
    image(std::exchange(other.image, VK_NULL_HANDLE)),
    allocation(std::exchange(other.allocation, VK_NULL_HANDLE)),
    view(std::move(other.view)),
    description(other.description),
    state(other.state)
{
}

VulkanImage& VulkanImage::operator=(VulkanImage&& other) noexcept {
    if (this != &other) {
        release();
        allocator = other.allocator;
        image = std::exchange(other.image, VK_NULL_HANDLE);
        allocation = std::exchange(other.allocation, VK_NULL_HANDLE);
        view = std::move(other.view);
        description = other.description;
        state = other.state;
    }

    return *this;
}

// The view goes before the image it looks into.
void VulkanImage::release() noexcept {
    view.reset();

    if (image != VK_NULL_HANDLE) {
        vmaDestroyImage(allocator, image, allocation);
        image = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }

    state = ImageState::Undefined;
}

VkImage VulkanImage::get() const {
    return image;
}

VkImageView VulkanImage::getView() const {
    return view.get();
}

const ImageDescription& VulkanImage::getDescription() const {
    return description;
}

ImageState VulkanImage::getState() const {
    return state;
}

void VulkanImage::transition(VkCommandBuffer commands, ImageState to, ImageContents contents) {
    transitionImage(commands, image, description.aspect, state, to, contents);
}

void VulkanImage::setState(ImageState newState) {
    state = newState;
}

VulkanImage::operator bool() const {
    return image != VK_NULL_HANDLE;
}

void recordImageBarrier(VkCommandBuffer commands, const VkImageMemoryBarrier2& barrier) {
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commands, &dependency);
}

void transitionImage(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, ImageState& state, ImageState to,
                     ImageContents contents) {
    if (!needsBarrier(state, to)) {
        return;
    }

    recordImageBarrier(commands, imageBarrier(image, aspect, state, to, contents));
    state = to;
}
