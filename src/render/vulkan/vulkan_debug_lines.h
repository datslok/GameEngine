#pragma once

#include "math/mat4.h"
#include "render/vulkan/frame_context.h"
#include "render/vulkan/vulkan_memory.h"
#include "render/vulkan/vulkan_pipeline.h"
#include "scene/debug_draw.h"

#include <cstdint>
#include <memory>
#include <vector>

class VulkanDevice;

/*
* A frame's debug lines over the finished image, as DebugLineRenderer draws them: world lines through the camera, screen
* lines in window pixels, unlit and with no depth test, so they show through walls. Each frame in flight has its own
* vertex buffer in upload memory, written directly by the CPU (no copy pass), growing when a frame has more lines.
*/
class VulkanDebugLines {
public:
    VulkanDebugLines(const VulkanDevice& device, DescriptorLayouts& layouts, std::uint32_t framesInFlight);

    // The pipeline draws into images of this format (the screen's); remade when it changes.
    void setTargetFormat(VkFormat format);

    // Write this frame's lines, colours multiplied by colourScale (an HDR screen's white level).
    void upload(std::uint32_t frameIndex, const std::vector<DebugLine>& worldLines, const std::vector<DebugLine>& screenLines,
                float colourScale);

    // Draw the last upload into the open pass. Screen lines map (0, 0) to the top-left of a frame this size.
    void draw(FrameContext& frame, const Mat4& viewProjection, std::uint32_t frameWidth, std::uint32_t frameHeight);

private:
    const VulkanDevice& device;
    DescriptorLayouts& layouts;
    std::unique_ptr<VulkanPipeline> pipeline;
    VkFormat targetFormat = VK_FORMAT_UNDEFINED;

    struct FrameLines {
        VulkanBuffer vertices;
        std::uint32_t capacity = 0; // in vertices
    };

    std::vector<FrameLines> frames;
    std::uint32_t worldVertexCount = 0;
    std::uint32_t screenVertexCount = 0;

    void drawRange(FrameContext& frame, const Mat4& transform, std::uint32_t firstVertex, std::uint32_t vertexCount);
};
