#pragma once

#include "gpu_function_runner.h"

#include "math/vec2.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_memory.h"

#include <vector>

class VulkanDevice;

// A half-float image read back as floats, channels per texel.
struct ReadImage {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<float> values;

    // Texel (x, y), its channels in the first of four (the rest 0).
    GpuVec4 texel(int x, int y) const;

    /*
    * The image filtered at uv (0..1 across) as a linear, clamped sampler reads it: the four texels round the point,
    * weighted by its distance from their centres. GPUs keep the weights to about 8 bits, so expect differences of a few
    * thousandths where neighbouring texels differ much.
    */
    GpuVec4 bilinear(const Vec2& uv) const;
};

// Copy a half-float image (RGBA16F, RG16F...) of channels per texel back to the CPU, leaving it a transfer source.
ReadImage readHalfImage(const VulkanDevice& device, ImmediateCommands& commands, VulkanImage& image, int channels);

// Copy a GPU buffer (made with VK_BUFFER_USAGE_TRANSFER_SRC_BIT) back to the CPU as floats.
std::vector<float> readFloatBuffer(const VulkanDevice& device, ImmediateCommands& commands, const VulkanBuffer& buffer, std::size_t floats);
