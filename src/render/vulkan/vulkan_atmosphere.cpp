#include "render/vulkan/vulkan_atmosphere.h"
#include "render/gpu/compute_dispatch.h"
#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"

#include <array>
#include <span>
#include <vector>

namespace {
    constexpr VkFormat tableFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // What the flat ambient buffer holds: one vec4.
    constexpr VkDeviceSize skyLightBytes = 4 * sizeof(float);

    // The first two tables depend on the air and the planet only, not on the sun, the camera or the disc.
    bool sameAir(const Atmosphere& a, const Atmosphere& b) {
        const auto same = [](const Vec3& x, const Vec3& y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
        return a.bottomRadius == b.bottomRadius && a.topRadius == b.topRadius && same(a.rayleighScattering, b.rayleighScattering) &&
               a.rayleighScaleHeight == b.rayleighScaleHeight && a.mieScattering == b.mieScattering && a.mieAbsorption == b.mieAbsorption &&
               a.mieScaleHeight == b.mieScaleHeight && a.mieAnisotropy == b.mieAnisotropy && same(a.ozoneAbsorption, b.ozoneAbsorption) &&
               a.ozoneCentre == b.ozoneCentre && a.ozoneHalfWidth == b.ozoneHalfWidth && same(a.groundAlbedo, b.groundAlbedo);
    }

    /*
    * A compute pass's set 0 in its bindings' order (ComputePipeline): its storage buffers, then its sampled images, then
    * its storage images (in the general layout).
    */
    void writeComputeSet(VkDevice device, VkDescriptorSet set, std::span<const VkDescriptorBufferInfo> buffers, std::span<const SampledImage> sampled,
                         std::span<const VkImageView> storage) {
        std::vector<VkDescriptorImageInfo> imageInfos;
        imageInfos.reserve(sampled.size() + storage.size());
        std::vector<VkWriteDescriptorSet> writes;
        std::uint32_t binding = 0;

        const auto write = [&](VkDescriptorType type) {
            VkWriteDescriptorSet entry{};
            entry.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            entry.dstSet = set;
            entry.dstBinding = binding++;
            entry.descriptorCount = 1;
            entry.descriptorType = type;
            return entry;
        };

        for (const VkDescriptorBufferInfo& buffer : buffers) {
            VkWriteDescriptorSet entry = write(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
            entry.pBufferInfo = &buffer;
            writes.push_back(entry);
        }

        for (const SampledImage& image : sampled) {
            imageInfos.push_back(VkDescriptorImageInfo{image.sampler, image.view, image.layout});
            VkWriteDescriptorSet entry = write(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
            entry.pImageInfo = &imageInfos.back();
            writes.push_back(entry);
        }

        for (const VkImageView view : storage) {
            imageInfos.push_back(VkDescriptorImageInfo{VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL});
            VkWriteDescriptorSet entry = write(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
            entry.pImageInfo = &imageInfos.back();
            writes.push_back(entry);
        }

        vkUpdateDescriptorSets(device, static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    VulkanImage createTable(const VulkanDevice& device, int width, int height, const char* name) {
        ImageDescription description;
        description.format = tableFormat;
        description.width = static_cast<std::uint32_t>(width);
        description.height = static_cast<std::uint32_t>(height);
        // Copied from by the GPU tests, which read the tables back.
        description.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        description.name = name;
        return VulkanImage{device, description};
    }

    // One pass over a set of the given buffers and images.
    void dispatch(VkCommandBuffer commands, FrameDescriptors& descriptors, VkDevice device, const ComputePipeline& pipeline,
                  std::span<const VkDescriptorBufferInfo> buffers, std::span<const SampledImage> sampled, std::span<const VkImageView> storage,
                  std::uint32_t groupsX, std::uint32_t groupsY) {
        const VkDescriptorSet set = descriptors.allocate(pipeline.getResourceLayout());
        writeComputeSet(device, set, buffers, sampled, storage);
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.get());
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.getLayout(), 0, 1, &set, 0, nullptr);
        vkCmdDispatch(commands, groupsX, groupsY, 1);
    }
}

VulkanAtmosphere::VulkanAtmosphere(const VulkanDevice& device, DescriptorLayouts& layouts, VkSampler sampler, VkFormat colourFormat,
                                   VkFormat depthFormat):
    device(device),
    sampler(sampler),
    transmittance(createTable(device, transmittanceLutWidth, transmittanceLutHeight, "Atmosphere transmittance table")),
    multipleScattering(createTable(device, multipleScatteringLutSize, multipleScatteringLutSize, "Atmosphere multiple scattering table")),
    skyView(createTable(device, skyViewLutWidth, skyViewLutHeight, "Atmosphere sky view table")),
    // Copied from by the GPU tests, which read the ambient back.
    skyLight(device.getAllocator(), skyLightBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Gpu)
{
    ComputePipelineDescription tables;
    tables.storageBuffers = 1;
    tables.storageImages = 1;
    tables.shader = "assets/shaders/atmosphere_transmittance.comp.spv";
    transmittancePipeline = std::make_unique<ComputePipeline>(device, tables);
    tables.shader = "assets/shaders/atmosphere_multiple_scattering.comp.spv";
    multipleScatteringPipeline = std::make_unique<ComputePipeline>(device, tables);

    ComputePipelineDescription view;
    view.storageBuffers = 1;
    view.sampledImages = 2;
    view.storageImages = 1;
    view.shader = "assets/shaders/atmosphere_sky_view.comp.spv";
    skyViewPipeline = std::make_unique<ComputePipeline>(device, view);

    ComputePipelineDescription ambient;
    ambient.storageBuffers = 2;
    ambient.sampledImages = 1;
    ambient.shader = "assets/shaders/atmosphere_ambient.comp.spv";
    ambientPipeline = std::make_unique<ComputePipeline>(device, ambient);

    createSkyPipeline(layouts, colourFormat, depthFormat);
}

/*
* The sky draws the full-screen triangle at the far depth with the depth test equal, so it lands only where nothing was
* drawn, and adds itself to the background there. Its set: the atmosphere's numbers and the sky view table; its push
* constants: the camera's rays.
*/
void VulkanAtmosphere::createSkyPipeline(DescriptorLayouts& layouts, VkFormat colourFormat, VkFormat depthFormat) {
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0] = VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    setInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    setInfo.pBindings = bindings.data();
    VkDescriptorSetLayout createdSet = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorSetLayout(device.getDevice(), &setInfo, nullptr, &createdSet), "Sky set layout creation failed");
    skySetLayout = OwnedHandle<VkDescriptorSetLayout>{device.getDevice(), createdSet, vkDestroyDescriptorSetLayout};

    const VkDescriptorSetLayout setLayout = skySetLayout.get();
    const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GpuRayDebugView)};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &setLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    VkPipelineLayout createdLayout = VK_NULL_HANDLE;
    vulkanCheck(vkCreatePipelineLayout(device.getDevice(), &layoutInfo, nullptr, &createdLayout), "Sky layout creation failed");
    skyLayout = OwnedHandle<VkPipelineLayout>{device.getDevice(), createdLayout, vkDestroyPipelineLayout};

    PipelineDescription description;
    description.vertexShader = "assets/shaders/fullscreen_triangle.vert.spv";
    description.fragmentShader = "assets/shaders/sky.frag.spv";
    description.sharedLayout = skyLayout.get();
    description.colourFormat = colourFormat;
    description.blend = BlendMode::Additive;
    description.depthFormat = depthFormat;
    description.depthTest = true;
    description.depthWrite = false;
    description.depthCompare = VK_COMPARE_OP_EQUAL;
    skyPipeline = std::make_unique<VulkanPipeline>(device, layouts, description);
}

/*
* Both tables are written whole, so their old contents are discarded; they are then only read (by the sky view pass and,
* the transmittance, by nothing else), so they stay readable until the air changes.
*/
void VulkanAtmosphere::makeTables(VkCommandBuffer commands, FrameDescriptors& descriptors, const VkDescriptorBufferInfo& params) {
    const std::array<VkDescriptorBufferInfo, 1> buffers{params};

    transmittance.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const std::array<VkImageView, 1> transmittanceView{transmittance.getView()};
    dispatch(commands, descriptors, device.getDevice(), *transmittancePipeline, buffers, {}, transmittanceView,
             workgroupCount(transmittanceLutWidth, ATMOSPHERE_GROUP_SIDE), workgroupCount(transmittanceLutHeight, ATMOSPHERE_GROUP_SIDE));
    transmittance.transition(commands, ImageState::SurfaceRead);

    multipleScattering.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const std::array<VkImageView, 1> scatteringView{multipleScattering.getView()};
    dispatch(commands, descriptors, device.getDevice(), *multipleScatteringPipeline, buffers, {}, scatteringView, multipleScatteringLutSize,
             multipleScatteringLutSize);
    multipleScattering.transition(commands, ImageState::SurfaceRead);
}

/*
* The sky view table goes to storage use from being sampled (last frame's sky, ambient and probe rays), whose reads must
* finish first, and back. The ambient buffer is read by last frame's main pass and probe rays: its write waits for those
* reads, and this frame's reads wait for the write.
*/
void VulkanAtmosphere::update(VkCommandBuffer commands, FrameDescriptors& descriptors, const Atmosphere& atmosphere, const VkDescriptorBufferInfo& params) {
    if (!tablesMadeFor || !sameAir(*tablesMadeFor, atmosphere)) {
        makeTables(commands, descriptors, params);
        tablesMadeFor = atmosphere;
    }

    const std::array<VkDescriptorBufferInfo, 1> buffers{params};
    const std::array<SampledImage, 2> tables{SampledImage{transmittance.getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                              SampledImage{multipleScattering.getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    skyView.transition(commands, ImageState::ComputeStorage, ImageContents::Discard);
    const std::array<VkImageView, 1> skyViewView{skyView.getView()};
    dispatch(commands, descriptors, device.getDevice(), *skyViewPipeline, buffers, tables, skyViewView,
             workgroupCount(skyViewLutWidth, ATMOSPHERE_GROUP_SIDE), workgroupCount(skyViewLutHeight, ATMOSPHERE_GROUP_SIDE));
    skyView.transition(commands, ImageState::SurfaceRead);

    const VkPipelineStageFlags2 readers = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    bufferBarrier(commands, skyLight.get(), {readers, VK_ACCESS_2_NONE}, {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT});
    const std::array<VkDescriptorBufferInfo, 2> ambientBuffers{params, describeSkyLight()};
    const std::array<SampledImage, 1> sky{sampledSkyView()};
    dispatch(commands, descriptors, device.getDevice(), *ambientPipeline, ambientBuffers, sky, {}, 1, 1);
    bufferBarrier(commands, skyLight.get(), {VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT},
                  {readers, VK_ACCESS_2_SHADER_STORAGE_READ_BIT});
}

void VulkanAtmosphere::drawSky(VkCommandBuffer commands, FrameDescriptors& descriptors, const VkDescriptorBufferInfo& params, const GpuRayDebugView& camera) {
    const VkDescriptorSet set = descriptors.allocate(skySetLayout.get());
    const SampledImage table = sampledSkyView();
    const VkDescriptorImageInfo imageInfo{table.sampler, table.view, table.layout};
    std::array<VkWriteDescriptorSet, 2> writes{};

    for (std::size_t index = 0; index < writes.size(); ++index) {
        writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[index].dstSet = set;
        writes[index].dstBinding = static_cast<std::uint32_t>(index);
        writes[index].descriptorCount = 1;
    }

    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &params;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &imageInfo;
    vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);

    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, skyPipeline->get());
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_GRAPHICS, skyLayout.get(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, skyLayout.get(), VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(camera), &camera);
    vkCmdDraw(commands, 3, 1, 0, 0);
}

SampledImage VulkanAtmosphere::sampledSkyView() const {
    return SampledImage{skyView.getView(), sampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}

VkDescriptorBufferInfo VulkanAtmosphere::describeSkyLight() const {
    return VkDescriptorBufferInfo{skyLight.get(), 0, VK_WHOLE_SIZE};
}

VulkanImage& VulkanAtmosphere::getTransmittanceTable() {
    return transmittance;
}

VulkanImage& VulkanAtmosphere::getMultipleScatteringTable() {
    return multipleScattering;
}

VulkanImage& VulkanAtmosphere::getSkyViewTable() {
    return skyView;
}

const VulkanBuffer& VulkanAtmosphere::getSkyLight() const {
    return skyLight;
}
