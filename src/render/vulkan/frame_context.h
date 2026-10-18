#pragma once

#include "volk/volk.h"

#include <cstdint>

class FrameDescriptors;
class FrameUniforms;

// What a pass records into: the frame's command buffer, and the frame's uniform data and descriptor sets.
struct FrameContext {
    VkCommandBuffer commands = VK_NULL_HANDLE;
    FrameUniforms& uniforms;
    FrameDescriptors& descriptors;
    std::uint32_t frameIndex = 0;
};
