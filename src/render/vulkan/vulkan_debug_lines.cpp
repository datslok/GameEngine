#include "render/vulkan/vulkan_debug_lines.h"
#include "render/gpu/debug_line_vertices.h"
#include "render/gpu/gpu_depth_range.h"
#include "render/gpu/column_major.h"
#include "render/vulkan/frame_descriptors.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

VulkanDebugLines::VulkanDebugLines(const VulkanDevice& device, DescriptorLayouts& layouts, std::uint32_t framesInFlight):
    device(device),
    layouts(layouts),
    frames(framesInFlight)
{
}

// The buffers start again empty (they grow on the next upload). The GPU must be idle: it may still read the old ones.
void VulkanDebugLines::setFramesInFlight(std::uint32_t framesInFlight) {
    frames = std::vector<FrameLines>(framesInFlight);
}

// A line list: every two vertices one line.
void VulkanDebugLines::setTargetFormat(VkFormat format) {
    if (format == targetFormat && pipeline) {
        return;
    }

    PipelineDescription description;
    description.vertexShader = "assets/shaders/debug_line.vert.spv";
    description.fragmentShader = "assets/shaders/debug_line.frag.spv";
    description.resources.vertexUniforms = 1;
    description.vertices.stride = sizeof(DebugLineVertex);
    description.vertices.attributes = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(DebugLineVertex, position))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, static_cast<std::uint32_t>(offsetof(DebugLineVertex, colour))},
    };
    description.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    description.colourFormat = format;

    pipeline = std::make_unique<VulkanPipeline>(device, layouts, description);
    targetFormat = format;
}

/*
* World and screen lines share one buffer, world lines first. The buffer only grows, doubling, so a slowly growing count
* does not reallocate every frame; it belongs to this frame alone, whose earlier use the GPU has finished.
*/
void VulkanDebugLines::upload(std::uint32_t frameIndex, const std::vector<DebugLine>& worldLines, const std::vector<DebugLine>& screenLines,
                              float colourScale) {
    worldVertexCount = 0;
    screenVertexCount = 0;

    std::vector<DebugLineVertex> vertices = buildDebugLineVertices(worldLines, colourScale);
    const std::vector<DebugLineVertex> screenVertices = buildDebugLineVertices(screenLines, colourScale);
    const auto worldCount = static_cast<std::uint32_t>(vertices.size());
    vertices.insert(vertices.end(), screenVertices.begin(), screenVertices.end());

    if (vertices.empty()) {
        return;
    }

    FrameLines& lines = frames.at(frameIndex);
    const auto vertexCount = static_cast<std::uint32_t>(vertices.size());

    if (vertexCount > lines.capacity) {
        const std::uint32_t capacity = std::max({vertexCount, lines.capacity * 2, std::uint32_t{1024}});
        lines.vertices = VulkanBuffer{device.getAllocator(), VkDeviceSize{capacity} * sizeof(DebugLineVertex),
                                      VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, BufferMemory::Upload};
        lines.capacity = capacity;
    }

    std::memcpy(lines.vertices.getMapped(), vertices.data(), vertices.size() * sizeof(DebugLineVertex));
    lines.vertices.flush();

    worldVertexCount = worldCount;
    screenVertexCount = vertexCount - worldCount;
}

/*
* World lines go through the camera; screen lines are already in window pixels, so a flat projection maps (0, 0) to the
* top-left corner and (width, height) to the bottom-right.
*/
void VulkanDebugLines::draw(FrameContext& frame, const Mat4& viewProjection, std::uint32_t frameWidth, std::uint32_t frameHeight) {
    if (worldVertexCount > 0) {
        drawRange(frame, viewProjection, 0, worldVertexCount);
    }

    if (screenVertexCount > 0) {
        const Mat4 pixels = toGpuDepthRange(Mat4::orthographic(0.0f, static_cast<float>(frameWidth), static_cast<float>(frameHeight), 0.0f, -1.0f, 1.0f));
        drawRange(frame, pixels, worldVertexCount, screenVertexCount);
    }
}

void VulkanDebugLines::drawRange(FrameContext& frame, const Mat4& transform, std::uint32_t firstVertex, std::uint32_t vertexCount) {
    const VkCommandBuffer commands = frame.commands;
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline->get());

    const VkBuffer buffer = frames.at(frame.frameIndex).vertices.get();
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commands, 0, 1, &buffer, &offset);

    float matrix[16]{};
    writeColumnMajor(transform, matrix);
    frame.uniforms.push(VK_SHADER_STAGE_VERTEX_BIT, 0, matrix, sizeof(matrix));
    frame.uniforms.bind(commands, *pipeline);

    vkCmdDraw(commands, vertexCount, 1, firstVertex, 0);
}
