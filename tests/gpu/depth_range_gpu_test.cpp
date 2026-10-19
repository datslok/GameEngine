#include "gpu_check.h"

#include "render/gpu/depth_range.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_depth_range.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_memory.h"

#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {
    // One depth image to measure: its size, and its depths row by row (0 where the sky shows).
    struct DepthCase {
        std::string name;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        std::vector<float> depths;
    };

    // Random surfaces, with sky in about a fifth of the pixels.
    DepthCase randomDepths(const char* name, std::uint32_t width, std::uint32_t height, std::uint32_t seed) {
        CaseRandom random{seed};
        DepthCase depthCase{name, width, height, {}};

        for (std::uint32_t pixel = 0; pixel < width * height; ++pixel) {
            const bool sky = random.between(0.0f, 1.0f) < 0.2f;
            depthCase.depths.push_back(sky ? 0.0f : random.between(1e-6f, 1.0f));
        }

        return depthCase;
    }

    /*
    * The real pass (VulkanDepthRange, depth_range.comp) on a depth image of known values, uploaded through a buffer: the
    * range it reads back must be the CPU's range of the same depths, bit for bit, since only comparisons are involved.
    * The sizes are not whole groups, so groups cut off by the image's edge are covered, and so are odd corners: the only
    * surface in the last pixel, the nearest plane, the smallest depth above the sky, and nothing but sky.
    */
    void checkCase(const VulkanDevice& device, ImmediateCommands& commands, VulkanDepthRange& depthRange, FrameDescriptors& descriptors,
                   const DepthCase& depthCase, GpuCheckReport& report) {
        const VkDeviceSize bytes = depthCase.depths.size() * sizeof(float);
        VulkanBuffer staging{device.getAllocator(), bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload};
        std::memcpy(staging.getMapped(), depthCase.depths.data(), bytes);
        staging.flush();

        ImageDescription description;
        description.format = VK_FORMAT_D32_SFLOAT;
        description.width = depthCase.width;
        description.height = depthCase.height;
        // A depth attachment, as the scene's depth is: the read-only depth layout the pass reads it in needs that.
        description.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        description.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        description.name = "Depth range test";
        VulkanImage image{device, description};

        descriptors.reset();
        commands.run([&](VkCommandBuffer recording) {
            image.transition(recording, ImageState::TransferDestination, ImageContents::Discard);

            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {depthCase.width, depthCase.height, 1};
            vkCmdCopyBufferToImage(recording, staging.get(), image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            depthRange.measure(recording, descriptors, 0, image);
        });

        ScreenDepthRange expected;

        for (float depth : depthCase.depths) {
            expected = includeDepth(expected, depth);
        }

        const std::optional<ScreenDepthRange> measured = depthRange.collect(0);
        report.check("measured " + depthCase.name, exactTolerance, 1.0f, measured.has_value() ? 1.0f : 0.0f);

        if (!measured) {
            return;
        }

        report.check("surfaces found", exactTolerance, hasSurfaces(expected) ? 1.0f : 0.0f, hasSurfaces(*measured) ? 1.0f : 0.0f);

        if (hasSurfaces(expected)) {
            report.check("farthest surface", exactTolerance, farthestSurfaceDepth(expected), farthestSurfaceDepth(*measured));
            report.check("nearest surface", exactTolerance, nearestSurfaceDepth(expected), nearestSurfaceDepth(*measured));
        }

        // Read once: the next collect has nothing new.
        report.check("collected once", exactTolerance, 0.0f, depthRange.collect(0).has_value() ? 1.0f : 0.0f);
    }
}

void gpuTestDepthRange(const VulkanDevice& device, GpuCheckReport& report) {
    report.section("depth_range.comp (the cascades' fit)");

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    const OwnedHandle<VkSampler> sampler = device.createSampler(samplerInfo);

    ImmediateCommands commands{device};
    FrameDescriptors descriptors{device.getDevice()};
    VulkanDepthRange depthRange{device, sampler.get(), 1};

    std::vector<DepthCase> cases{
        randomDepths("1x1", 1, 1, 31),
        randomDepths("37x23", 37, 23, 32),
        randomDepths("64x64", 64, 64, 33),
        randomDepths("301x173", 301, 173, 34),
        randomDepths("1280x720", 1280, 720, 35),
    };

    DepthCase lastPixel{"last pixel only", 97, 61, std::vector<float>(97 * 61, 0.0f)};
    lastPixel.depths.back() = 0.0123f;
    cases.push_back(lastPixel);

    // The near plane and the smallest normal float. Not a denormal: depth images may flush those to zero (the sky), and
    // a real depth never comes near one (the far plane's are about 1e-4).
    DepthCase extremes{"extremes", 40, 9, std::vector<float>(40 * 9, 0.5f)};
    extremes.depths[3] = 1.0f;
    extremes.depths[200] = std::numeric_limits<float>::min();
    cases.push_back(extremes);

    cases.push_back(DepthCase{"sky only", 50, 40, std::vector<float>(50 * 40, 0.0f)});

    for (const DepthCase& depthCase : cases) {
        checkCase(device, commands, depthRange, descriptors, depthCase, report);
    }
}
