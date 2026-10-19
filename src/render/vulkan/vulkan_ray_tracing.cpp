#include "render/vulkan/vulkan_ray_tracing.h"
#include "render/vulkan/immediate_commands.h"
#include "render/vulkan/vulkan_check.h"
#include "render/vulkan/vulkan_device.h"
#include "render/vulkan/vulkan_mesh.h"
#include "scene/indexed_mesh.h"

#include <algorithm>
#include <array>
#include <stdexcept>

static_assert(sizeof(VkAccelerationStructureInstanceKHR) == sizeof(GpuRayInstance),
              "GpuRayInstance must have VkAccelerationStructureInstanceKHR's layout");

namespace {
    // One mesh's buffers by address, as ray_scene.glsl's MeshAddresses.
    struct GpuMeshAddresses {
        VkDeviceAddress vertices = 0;
        VkDeviceAddress indices = 0;
    };

    // A frame's top-level structure starts with room for this many instances and doubles as needed.
    constexpr std::uint32_t smallestSceneCapacity = 256;

    // Everything later in the frame that can read an acceleration structure: building a structure on top of it, and ray
    // queries in fragment and compute shaders.
    void structureBarrier(VkCommandBuffer commands, VkPipelineStageFlags2 laterStages) {
        VkMemoryBarrier2 barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
        barrier.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
        barrier.dstStageMask = laterStages;
        barrier.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

        VkDependencyInfo dependency{};
        dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
        dependency.memoryBarrierCount = 1;
        dependency.pMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(commands, &dependency);
    }

    constexpr VkPipelineStageFlags2 rayQueryStages = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
}

VulkanRayTracing::VulkanRayTracing(const VulkanDevice& device, std::uint32_t framesInFlight):
    device(device)
{
    if (!device.supportsRayQueries()) {
        throw std::logic_error("VulkanRayTracing needs a GPU with ray queries");
    }

    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    bindings[1].binding = 1;
    bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<std::uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    VkDescriptorSetLayout created = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorSetLayout(device.getDevice(), &layoutInfo, nullptr, &created), "Ray tracing set layout creation failed");
    setLayout = OwnedHandle<VkDescriptorSetLayout>{device.getDevice(), created, vkDestroyDescriptorSetLayout};

    setFramesInFlight(framesInFlight);
}

/*
* One set per frame in flight, from a pool of exactly that many, rewritten each frame by buildScene (the frame's fence has
* passed by then, so nothing reads it).
*/
void VulkanRayTracing::setFramesInFlight(std::uint32_t count) {
    frames.clear();
    pool.reset();

    const std::array<VkDescriptorPoolSize, 2> sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, count},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, count},
    };

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = count;
    poolInfo.poolSizeCount = static_cast<std::uint32_t>(sizes.size());
    poolInfo.pPoolSizes = sizes.data();

    VkDescriptorPool createdPool = VK_NULL_HANDLE;
    vulkanCheck(vkCreateDescriptorPool(device.getDevice(), &poolInfo, nullptr, &createdPool), "Ray tracing descriptor pool creation failed");
    pool = OwnedHandle<VkDescriptorPool>{device.getDevice(), createdPool, vkDestroyDescriptorPool};

    for (std::uint32_t index = 0; index < count; ++index) {
        Frame frame;
        frame.instances = std::make_unique<UploadArray>(
            device.getAllocator(), VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR);
        frame.meshTable = std::make_unique<UploadArray>(device.getAllocator(), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

        const VkDescriptorSetLayout layout = setLayout.get();
        VkDescriptorSetAllocateInfo allocateInfo{};
        allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocateInfo.descriptorPool = pool.get();
        allocateInfo.descriptorSetCount = 1;
        allocateInfo.pSetLayouts = &layout;
        vulkanCheck(vkAllocateDescriptorSets(device.getDevice(), &allocateInfo, &frame.set), "Ray tracing set allocation failed");

        frames.push_back(std::move(frame));
    }
}

VulkanRayTracing::AccelerationStructure VulkanRayTracing::createStructure(VkAccelerationStructureTypeKHR type, VkDeviceSize size) const {
    AccelerationStructure structure;
    structure.storage = VulkanBuffer{device.getAllocator(), size,
                                     VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                                     BufferMemory::Gpu};

    VkAccelerationStructureCreateInfoKHR info{};
    info.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
    info.buffer = structure.storage.get();
    info.size = size;
    info.type = type;

    VkAccelerationStructureKHR created = VK_NULL_HANDLE;
    vulkanCheck(vkCreateAccelerationStructureKHR(device.getDevice(), &info, nullptr, &created), "Acceleration structure creation failed");
    structure.handle = OwnedHandle<VkAccelerationStructureKHR>{device.getDevice(), created, vkDestroyAccelerationStructureKHR};

    VkAccelerationStructureDeviceAddressInfoKHR addressInfo{};
    addressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
    addressInfo.accelerationStructure = created;
    structure.address = vkGetAccelerationStructureDeviceAddressKHR(device.getDevice(), &addressInfo);
    return structure;
}

// Scratch memory must start at a multiple of the GPU's alignment, which an allocation need not: one alignment's worth
// extra leaves room to start at the next multiple (scratchAddress).
VulkanBuffer VulkanRayTracing::createScratch(VkDeviceSize size) const {
    return VulkanBuffer{device.getAllocator(), size + device.getScratchAlignment(),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, BufferMemory::Gpu};
}

VkDeviceAddress VulkanRayTracing::scratchAddress(const VulkanBuffer& scratch) const {
    const VkDeviceAddress alignment = device.getScratchAlignment();
    return (scratch.getDeviceAddress() + alignment - 1) / alignment * alignment;
}

/*
* The mesh's triangles, opaque (no shader is asked whether a hit counts), positions read from each MeshVertex's first
* three floats. Built preferring fast tracing and allowing compaction; the build writes its compacted size into a query,
* read once the GPU has finished, and a second submission copies the tree into a buffer of exactly that size. The last
* barrier makes the compacted structure readable by top-level builds and ray queries in later submissions.
*/
void VulkanRayTracing::addMesh(ImmediateCommands& uploads, const VulkanMesh& mesh) {
    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
    geometry.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
    geometry.geometry.triangles.vertexData.deviceAddress = mesh.getVertexAddress();
    geometry.geometry.triangles.vertexStride = sizeof(MeshVertex);
    geometry.geometry.triangles.maxVertex = mesh.getVertexCount() - 1;
    geometry.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    geometry.geometry.triangles.indexData.deviceAddress = mesh.getIndexAddress();

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geometry;

    const std::uint32_t triangles = mesh.getIndexCount() / 3;
    VkAccelerationStructureBuildSizesInfoKHR sizes{};
    sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
    vkGetAccelerationStructureBuildSizesKHR(device.getDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &triangles, &sizes);

    AccelerationStructure built = createStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, sizes.accelerationStructureSize);
    const VulkanBuffer scratch = createScratch(sizes.buildScratchSize);
    build.dstAccelerationStructure = built.handle.get();
    build.scratchData.deviceAddress = scratchAddress(scratch);

    VkQueryPoolCreateInfo queryInfo{};
    queryInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryInfo.queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR;
    queryInfo.queryCount = 1;
    VkQueryPool createdPool = VK_NULL_HANDLE;
    vulkanCheck(vkCreateQueryPool(device.getDevice(), &queryInfo, nullptr, &createdPool), "Compaction query creation failed");
    const OwnedHandle<VkQueryPool> query{device.getDevice(), createdPool, vkDestroyQueryPool};

    const VkAccelerationStructureBuildRangeInfoKHR range{triangles, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    const VkAccelerationStructureKHR builtHandle = built.handle.get();

    uploads.run([&](VkCommandBuffer commands) {
        vkCmdResetQueryPool(commands, query.get(), 0, 1);
        vkCmdBuildAccelerationStructuresKHR(commands, 1, &build, &ranges);
        // Reading the finished tree's size waits for the build.
        structureBarrier(commands, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR);
        vkCmdWriteAccelerationStructuresPropertiesKHR(commands, 1, &builtHandle, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                                      query.get(), 0);
    });

    VkDeviceSize compactedSize = 0;
    vulkanCheck(vkGetQueryPoolResults(device.getDevice(), query.get(), 0, 1, sizeof(compactedSize), &compactedSize, sizeof(compactedSize),
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT),
                "Reading an acceleration structure's compacted size failed");

    MeshStructure entry;
    entry.structure = createStructure(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, compactedSize);
    entry.vertices = mesh.getVertexAddress();
    entry.indices = mesh.getIndexAddress();

    uploads.run([&](VkCommandBuffer commands) {
        VkCopyAccelerationStructureInfoKHR copy{};
        copy.sType = VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR;
        copy.src = builtHandle;
        copy.dst = entry.structure.handle.get();
        copy.mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR;
        vkCmdCopyAccelerationStructureKHR(commands, &copy);
        structureBarrier(commands, VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR | rayQueryStages);
    });

    meshes.push_back(std::move(entry));
}

std::size_t VulkanRayTracing::getMeshCount() const {
    return meshes.size();
}

std::vector<GpuRayInstance> VulkanRayTracing::packInstances(const std::vector<DrawItem>& draws) const {
    std::vector<GpuRayInstance> instances;
    instances.reserve(draws.size());

    for (std::size_t index = 0; index < draws.size(); ++index) {
        const DrawItem& draw = draws[index];

        if (draw.mesh.index >= meshes.size()) {
            throw std::logic_error("A drawn mesh has no acceleration structure: add every mesh before drawing it");
        }

        instances.push_back(packRayInstance(draw, static_cast<std::uint32_t>(index), meshes[draw.mesh.index].structure.address));
    }

    return instances;
}

/*
* The instances and the mesh table go into this frame's upload buffers (the CPU's writes are visible to the GPU once the
* commands are submitted), then the build reads the instances by address. A frame with more instances than its structure
* was sized for gets a bigger one first; the old one is unused, since this frame's last use of it finished before its
* fence. The barrier after the build makes the structure readable by this frame's ray queries.
*/
void VulkanRayTracing::buildScene(VkCommandBuffer commands, std::uint32_t frameIndex, const std::vector<GpuRayInstance>& instances) {
    Frame& frame = frames.at(frameIndex);
    const std::uint32_t count = static_cast<std::uint32_t>(instances.size());

    std::vector<GpuMeshAddresses> table;
    table.reserve(meshes.size());

    for (const MeshStructure& mesh : meshes) {
        table.push_back(GpuMeshAddresses{mesh.vertices, mesh.indices});
    }

    frame.meshTable->write(table);
    frame.instances->write(instances);

    VkAccelerationStructureGeometryKHR geometry{};
    geometry.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
    geometry.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
    geometry.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geometry.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
    geometry.geometry.instances.arrayOfPointers = VK_FALSE;
    geometry.geometry.instances.data.deviceAddress = frame.instances->getDeviceAddress();

    VkAccelerationStructureBuildGeometryInfoKHR build{};
    build.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
    build.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
    build.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    build.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    build.geometryCount = 1;
    build.pGeometries = &geometry;

    if (!frame.scene.handle.get() || count > frame.capacity) {
        frame.capacity = std::max({count, 2 * frame.capacity, smallestSceneCapacity});
        VkAccelerationStructureBuildSizesInfoKHR sizes{};
        sizes.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
        vkGetAccelerationStructureBuildSizesKHR(device.getDevice(), VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, &frame.capacity, &sizes);
        frame.scene = createStructure(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, sizes.accelerationStructureSize);
        frame.scratch = createScratch(sizes.buildScratchSize);
    }

    build.dstAccelerationStructure = frame.scene.handle.get();
    build.scratchData.deviceAddress = scratchAddress(frame.scratch);

    const VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    vkCmdBuildAccelerationStructuresKHR(commands, 1, &build, &ranges);
    structureBarrier(commands, rayQueryStages);

    const VkAccelerationStructureKHR scene = frame.scene.handle.get();
    VkWriteDescriptorSetAccelerationStructureKHR structureWrite{};
    structureWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR;
    structureWrite.accelerationStructureCount = 1;
    structureWrite.pAccelerationStructures = &scene;

    const VkDescriptorBufferInfo tableInfo = frame.meshTable->describe();
    std::array<VkWriteDescriptorSet, 2> writes{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].pNext = &structureWrite;
    writes[0].dstSet = frame.set;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = frame.set;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[1].pBufferInfo = &tableInfo;
    vkUpdateDescriptorSets(device.getDevice(), static_cast<std::uint32_t>(writes.size()), writes.data(), 0, nullptr);
}

VkDescriptorSet VulkanRayTracing::getSceneSet(std::uint32_t frameIndex) const {
    return frames.at(frameIndex).set;
}

VkDescriptorSetLayout VulkanRayTracing::getSetLayout() const {
    return setLayout.get();
}
