#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
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

VkDeviceAddress VulkanBuffer::getDeviceAddress() const {
    VmaAllocatorInfo allocatorInfo{};
    vmaGetAllocatorInfo(allocator, &allocatorInfo);

    VkBufferDeviceAddressInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
    info.buffer = buffer;
    return vkGetBufferDeviceAddress(allocatorInfo.device, &info);
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
    if (description.width == 0 || description.height == 0 || description.levels == 0 || description.layers == 0) {
        throw std::invalid_argument("Image dimensions, level and layer counts must be positive");
    }

    if (description.cube && (description.layers != 6 || description.width != description.height)) {
        throw std::invalid_argument("A cube image needs six square layers");
    }

    if (description.depth == 0 || (description.depth > 1 && !description.volume) || (description.volume && (description.layers != 1 || description.cube))) {
        throw std::invalid_argument("Only a volume has depth, and a volume has one layer");
    }

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.flags = description.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    imageInfo.imageType = description.volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    imageInfo.format = description.format;
    imageInfo.extent = {description.width, description.height, description.depth};
    imageInfo.mipLevels = description.levels;
    imageInfo.arrayLayers = description.layers;
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
        viewInfo.viewType = description.volume ? VK_IMAGE_VIEW_TYPE_3D
                            : description.cube ? VK_IMAGE_VIEW_TYPE_CUBE
                            : description.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                     : VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = description.format;
        viewInfo.subresourceRange.aspectMask = description.aspect;
        viewInfo.subresourceRange.levelCount = description.levels;
        viewInfo.subresourceRange.layerCount = description.layers;

        VkImageView created = VK_NULL_HANDLE;
        vulkanCheck(vkCreateImageView(device.getDevice(), &viewInfo, nullptr, &created), "Image view creation failed");
        view = OwnedHandle<VkImageView>{device.getDevice(), created, vkDestroyImageView};

        if (description.name != nullptr) {
            device.nameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<std::uint64_t>(image), description.name);
        }
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

/*
* Every level gets its own barriers: it is written (transfer destination), then read for the next level (transfer
* source), then left in the final state. The last level is never a source, so it goes straight to the final state.
*/
void generateMipmaps(VkCommandBuffer commands, VulkanImage& image, ImageState levelZero, ImageState finalState) {
    const VkImage handle = image.get();
    const ImageDescription& description = image.getDescription();
    const std::uint32_t levels = description.levels;
    const VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;

    if (levels > 1) {
        recordImageBarrier(commands, imageBarrier(handle, aspect, ImageState::Undefined, ImageState::TransferDestination, ImageContents::Discard, 1,
                                                  levels - 1));
    }

    for (std::uint32_t level = 1; level < levels; ++level) {
        const std::uint32_t source = level - 1;
        recordImageBarrier(commands, imageBarrier(handle, aspect, source == 0 ? levelZero : ImageState::TransferDestination, ImageState::TransferSource,
                                                  ImageContents::Keep, source, 1));

        VkImageBlit blit{};
        blit.srcSubresource = {aspect, source, 0, description.layers};
        blit.srcOffsets[1] = {static_cast<std::int32_t>(std::max(description.width >> source, 1u)),
                              static_cast<std::int32_t>(std::max(description.height >> source, 1u)), 1};
        blit.dstSubresource = {aspect, level, 0, description.layers};
        blit.dstOffsets[1] = {static_cast<std::int32_t>(std::max(description.width >> level, 1u)),
                              static_cast<std::int32_t>(std::max(description.height >> level, 1u)), 1};
        vkCmdBlitImage(commands, handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, handle, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        recordImageBarrier(commands, imageBarrier(handle, aspect, ImageState::TransferSource, finalState, ImageContents::Keep, source, 1));
    }

    recordImageBarrier(commands, imageBarrier(handle, aspect, levels > 1 ? ImageState::TransferDestination : levelZero, finalState,
                                              ImageContents::Keep, levels - 1, 1));
    image.setState(finalState);
}

void computeLevelWritten(VkCommandBuffer commands, const VulkanImage& image, std::uint32_t level) {
    VkImageMemoryBarrier2 barrier = imageBarrier(image.get(), VK_IMAGE_ASPECT_COLOR_BIT, ImageState::ComputeStorage, ImageState::ComputeStorage,
                                                 ImageContents::Keep, level, 1);
    barrier.dstAccessMask |= VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    recordImageBarrier(commands, barrier);
}

std::vector<OwnedHandle<VkImageView>> imageLevelViews(VkDevice device, const VulkanImage& image) {
    std::vector<OwnedHandle<VkImageView>> views;
    const ImageDescription& description = image.getDescription();

    for (std::uint32_t level = 0; level < description.levels; ++level) {
        VkImageViewCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = image.get();
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = description.format;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = level;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.layerCount = 1;

        VkImageView view = VK_NULL_HANDLE;
        vulkanCheck(vkCreateImageView(device, &info, nullptr, &view), "Pyramid level view creation failed");
        views.emplace_back(device, view, vkDestroyImageView);
    }

    return views;
}

void transitionImage(VkCommandBuffer commands, VkImage image, VkImageAspectFlags aspect, ImageState& state, ImageState to,
                     ImageContents contents) {
    if (!needsBarrier(state, to)) {
        return;
    }

    recordImageBarrier(commands, imageBarrier(image, aspect, state, to, contents));
    state = to;
}
