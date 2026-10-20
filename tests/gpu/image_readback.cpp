#include "image_readback.h"

#include "core/half_float.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

GpuVec4 ReadImage::texel(int x, int y) const {
    GpuVec4 value{0.0f, 0.0f, 0.0f, 0.0f};

    for (int channel = 0; channel < channels; ++channel) {
        value[static_cast<std::size_t>(channel)] = values[static_cast<std::size_t>((y * width + x) * channels + channel)];
    }

    return value;
}

GpuVec4 ReadImage::bilinear(const Vec2& uv) const {
    const float x = uv.x * static_cast<float>(width) - 0.5f;
    const float y = uv.y * static_cast<float>(height) - 0.5f;
    const float left = std::floor(x);
    const float top = std::floor(y);
    const float across = x - left;
    const float down = y - top;
    const auto at = [&](float column, float row) {
        return texel(std::clamp(static_cast<int>(column), 0, width - 1), std::clamp(static_cast<int>(row), 0, height - 1));
    };

    const GpuVec4 a = at(left, top);
    const GpuVec4 b = at(left + 1.0f, top);
    const GpuVec4 c = at(left, top + 1.0f);
    const GpuVec4 d = at(left + 1.0f, top + 1.0f);
    GpuVec4 value{};

    for (std::size_t channel = 0; channel < 4; ++channel) {
        const float upper = a[channel] + (b[channel] - a[channel]) * across;
        const float lower = c[channel] + (d[channel] - c[channel]) * across;
        value[channel] = upper + (lower - upper) * down;
    }

    return value;
}

ReadImage readHalfImage(const VulkanDevice& device, ImmediateCommands& commands, VulkanImage& image, int channels) {
    const ImageDescription& description = image.getDescription();
    ReadImage read{static_cast<int>(description.width), static_cast<int>(description.height), channels, {}};
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(read.width) * read.height * channels * sizeof(std::uint16_t);
    VulkanBuffer readback{device.getAllocator(), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback};

    commands.run([&](VkCommandBuffer recording) {
        image.transition(recording, ImageState::TransferSource);
        VkBufferImageCopy copy{};
        copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copy.imageSubresource.layerCount = 1;
        copy.imageExtent = {description.width, description.height, 1};
        vkCmdCopyImageToBuffer(recording, image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.get(), 1, &copy);
        bufferBarrier(recording, readback.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                      {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
    });

    readback.invalidate();
    std::vector<std::uint16_t> halves(static_cast<std::size_t>(bytes / sizeof(std::uint16_t)));
    std::memcpy(halves.data(), readback.getMapped(), bytes);

    for (std::uint16_t half : halves) {
        read.values.push_back(halfToFloat(half));
    }

    return read;
}

std::vector<float> readFloatBuffer(const VulkanDevice& device, ImmediateCommands& commands, const VulkanBuffer& buffer, std::size_t floats) {
    const VkDeviceSize bytes = floats * sizeof(float);
    VulkanBuffer readback{device.getAllocator(), bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback};

    commands.run([&](VkCommandBuffer recording) {
        bufferBarrier(recording, buffer.get(), {VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT},
                      {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT});
        const VkBufferCopy copy{0, 0, bytes};
        vkCmdCopyBuffer(recording, buffer.get(), readback.get(), 1, &copy);
        bufferBarrier(recording, readback.get(), {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT},
                      {VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT});
    });

    readback.invalidate();
    std::vector<float> values(floats);
    std::memcpy(values.data(), readback.getMapped(), bytes);
    return values;
}
