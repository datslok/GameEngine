#pragma once

#include "render/vulkan/owned_handle.h"

#include "volk/volk.h"

#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

class VulkanDevice;

/*
* How many resources a pipeline's shaders read, in the descriptor layout the shaders were written for (SDL_GPU's, kept
* from the port so the pictures stayed the same): set 0 holds vertex textures (none so far), set 1 the vertex
* uniform blocks, set 2 the fragment textures followed by the fragment storage buffers, and set 3 the fragment
* uniform blocks. Uniform blocks are bound as dynamic uniform buffers: one descriptor per block, pointing into the
* frame's uniform buffer, with the offset given at each draw.
*/
struct ShaderResources {
    std::uint32_t vertexUniforms = 0;
    std::uint32_t fragmentSamplers = 0;
    std::uint32_t fragmentStorageBuffers = 0;
    std::uint32_t fragmentUniforms = 0;
};

inline constexpr std::uint32_t vertexUniformSet = 1;
inline constexpr std::uint32_t fragmentResourceSet = 2;
inline constexpr std::uint32_t fragmentUniformSet = 3;
inline constexpr std::uint32_t maxStageUniforms = 4;

/*
* Descriptor set layouts, made once and shared: every pipeline with the same counts uses the same layout object (two
* layouts defined identically would also be compatible, but sharing keeps the count of objects down).
*/
class DescriptorLayouts {
public:
    explicit DescriptorLayouts(VkDevice device);

    DescriptorLayouts(const DescriptorLayouts&) = delete;
    DescriptorLayouts& operator=(const DescriptorLayouts&) = delete;

    VkDescriptorSetLayout empty();

    // count dynamic uniform buffers at bindings 0, 1, 2... for one shader stage.
    VkDescriptorSetLayout uniforms(VkShaderStageFlags stage, std::uint32_t count);

    // Textures with their samplers at bindings 0.. samplers - 1, then storage buffers, for fragment shaders.
    VkDescriptorSetLayout fragmentResources(std::uint32_t samplers, std::uint32_t storageBuffers);

private:
    VkDevice device;
    std::map<std::tuple<int, std::uint32_t, std::uint32_t>, OwnedHandle<VkDescriptorSetLayout>> layouts;

    VkDescriptorSetLayout find(int kind, std::uint32_t first, std::uint32_t second, const std::vector<VkDescriptorSetLayoutBinding>& bindings);
};

enum class BlendMode {
    None,               // the new colour replaces what was there
    Additive,           // the new colour is added to it (bloom growing back up)
    PremultipliedAlpha  // the new colour over it, which shows through by one minus the new alpha (the debug UI)
};

// Vertices read from one buffer, each stride bytes; no attributes means the vertex shader makes its own (a full-screen triangle).
struct VertexLayout {
    std::uint32_t stride = 0;
    std::vector<VkVertexInputAttributeDescription> attributes;
};

// Everything a graphics pipeline fixes in advance. Viewport and scissor are set per pass instead.
struct PipelineDescription {
    const char* vertexShader = nullptr;
    const char* fragmentShader = nullptr;
    ShaderResources resources;
    VertexLayout vertices;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags cullMode = VK_CULL_MODE_NONE;

    VkFormat colourFormat = VK_FORMAT_UNDEFINED; // none: depth only
    BlendMode blend = BlendMode::None;

    VkFormat depthFormat = VK_FORMAT_UNDEFINED; // none: no depth target
    bool depthTest = false;
    bool depthWrite = false;
    VkCompareOp depthCompare = VK_COMPARE_OP_ALWAYS;
};

/*
* A graphics pipeline: the shaders and all fixed-function state compiled together by the driver, so switching to it is
* one bind. Built for dynamic rendering, so it names only the formats of the images it draws into.
*/
class VulkanPipeline {
public:
    VulkanPipeline(const VulkanDevice& device, DescriptorLayouts& layouts, const PipelineDescription& description);

    VkPipeline get() const;
    VkPipelineLayout getLayout() const;
    const ShaderResources& getResources() const;

    // Set 2's layout, for allocating the descriptor set of a draw's textures and storage buffers.
    VkDescriptorSetLayout getFragmentResourceLayout() const;

private:
    ShaderResources resources;
    VkDescriptorSetLayout fragmentResourceLayout = VK_NULL_HANDLE;
    OwnedHandle<VkPipelineLayout> layout;
    OwnedHandle<VkPipeline> pipeline;
};

/*
* SDL_GPU's viewport, which the shaders and matrices assume: y measured down from the top like the window, with the
* viewport flipped (negative height) so clip space y points up as in OpenGL and Direct3D. The scissor is the whole
* target, as SDL sets it when a pass begins.
*/
void setViewport(VkCommandBuffer commands, float x, float y, float width, float height);
void setScissor(VkCommandBuffer commands, std::uint32_t width, std::uint32_t height);
