#include "render/vulkan/vulkan_twig_drawing.h"

#include "render/vulkan/buffer_barrier.h"
#include "render/vulkan/vulkan_device.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <map>
#include <stdexcept>
#include <type_traits>

namespace {
    constexpr VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    // The most sources the cull pass takes: a workgroup row each, within the dispatch's y limit.
    constexpr std::uint32_t mostSources = 65535;

    // A region's most room (64 MB of entries for each); the room grows to fit up to here.
    constexpr std::uint32_t largestRoom = 1u << 22;

    // The regions buffer as twig_pass.glsl lays it out: the counts (TWIG_REGION_WORDS), the room, then the tally; the
    // read back copies the regions' counts, then the tally.
    constexpr VkDeviceSize countBytes = TWIG_REGION_WORDS * sizeof(std::uint32_t);
    constexpr VkDeviceSize readCountBytes = TWIG_REGIONS * sizeof(std::uint32_t);
    constexpr VkDeviceSize regionsTallyOffset = countBytes + 4 * sizeof(std::uint32_t);
    constexpr VkDeviceSize tallyBytes = TWIG_TALLY_WORDS * sizeof(std::uint32_t);

    // The most bricks, the most sources, the frame's bones stamp, whether far levels are drawn (twig_pass.glsl's TwigPush).
    constexpr std::uint32_t pushBytes = 16;

    constexpr BufferUse computeWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse computeReadWrite{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
    constexpr BufferUse indirectRead{VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT};
    constexpr BufferUse transferWrite{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    constexpr BufferUse transferRead{VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};

    std::unique_ptr<ComputePipeline> passPipeline(const VulkanDevice& device, const char* shader) {
        ComputePipelineDescription description;
        description.shader = shader;
        description.storageBuffers = TWIG_PASS_BINDINGS;
        description.pushConstantBytes = pushBytes;
        return std::make_unique<ComputePipeline>(device, description);
    }

    // What reads the entries and regions after the passes: the cluster passes (compute), the mesh shaders, the copy back.
    BufferUse drawingRead(const VulkanDevice& device) {
        BufferUse use{VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                      VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT};

        if (device.supportsMeshShaders()) {
            use.stages |= VK_PIPELINE_STAGE_2_MESH_SHADER_BIT_EXT;
        }

        return use;
    }
}

VulkanTwigDrawing::VulkanTwigDrawing(const VulkanDevice& device, std::uint32_t framesInFlight):
    device(device),
    sourcesPass(passPipeline(device, "assets/shaders/twig_sources.comp.spv")),
    cullPass(passPipeline(device, "assets/shaders/twig_cull.comp.spv"))
{
    setFramesInFlight(framesInFlight);
}

void VulkanTwigDrawing::setFramesInFlight(std::uint32_t count) {
    frames.clear();
    frames.resize(count);
}

/*
* The tables: the sets in the table's order, each its bricks (their twigs' runs moved past the sets before) and twigs
* (their prototypes numbered among every set's, first met first), each prototype's levels from its clusters in the pool.
*/
bool VulkanTwigDrawing::prepare(const PlacedTable& table, const std::function<MeshPlacement(MeshHandle)>& placement,
                                std::span<const GpuMeshCluster> clusters, std::uint32_t frameIndex) {
    if (table.twigSets.empty()) {
        return false;
    }

    // The frame's working buffers sized here, before the scene set points at its entries (a buffer grown after would leave
    // the set pointing at the old one).
    PerFrame& frame = frames.at(frameIndex);
    const VmaAllocator allocator = device.getAllocator();
    ensureBuffer(allocator, frame.sources, 8 + VkDeviceSize{mostSources} * 8, storage, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.dispatch, 3 * sizeof(std::uint32_t), storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.regions, regionsTallyOffset + tallyBytes, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 BufferMemory::Gpu);
    ensureBuffer(allocator, frame.entries, VkDeviceSize{room} * TWIG_REGIONS * 4 * sizeof(std::uint32_t), storage, BufferMemory::Gpu);
    ensureBuffer(allocator, frame.readback, readCountBytes + tallyBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, BufferMemory::Readback);
    frame.room = room;

    std::vector<MeshHandle> prototypeMeshes;
    std::vector<std::uint32_t> places;

    for (const TwigSet* set : table.twigSets) {
        for (const MeshHandle mesh : set->prototypes) {
            if (std::find(prototypeMeshes.begin(), prototypeMeshes.end(), mesh) == prototypeMeshes.end()) {
                const MeshPlacement where = placement(mesh);

                if (!where.pooled) {
                    throw std::invalid_argument("A twig prototype's mesh is not in the pools");
                }

                prototypeMeshes.push_back(mesh);
                places.insert(places.end(), {where.firstCluster, where.clusterCount});
            }
        }
    }

    twigGroups.clear();

    for (const GpuPlacedPart& part : table.parts) {
        if ((part.record.flags & CULL_TWIGS) != 0u) {
            for (std::uint32_t slot = 0; slot <= part.levels.levels; ++slot) {
                twigGroups.push_back(part.firstGroup + slot);
            }
        }
    }

    if (table.twigSets == madeFrom && places == madePlaces) {
        return true;
    }

    std::vector<GpuTwigPrototype> prototypeRecords;
    std::vector<GpuTwigLevel> levelRecords;

    for (std::size_t prototype = 0; prototype < prototypeMeshes.size(); ++prototype) {
        const std::uint32_t first = places[2 * prototype];
        const std::uint32_t count = places[2 * prototype + 1];
        const std::vector<GpuTwigLevel> levels = packTwigLevels(clusters.subspan(first, count), first);
        prototypeRecords.push_back(GpuTwigPrototype{static_cast<std::uint32_t>(levelRecords.size()), static_cast<std::uint32_t>(levels.size())});
        levelRecords.insert(levelRecords.end(), levels.begin(), levels.end());
    }

    PackedTwigSets packed = packTwigSets(table.twigSets, prototypeMeshes);
    mostBricks = 0;

    for (const GpuTwigSet& set : packed.sets) {
        mostBricks = std::max(mostBricks, set.brickCount);
    }

    // Every frame in flight reads the tables: none may be reading them while they are remade.
    if (sets.get() != VK_NULL_HANDLE) {
        device.waitIdle();
    }

    const std::vector<GpuTwigBrick>& brickRecords = packed.tables.bricks;
    const std::vector<GpuTwig>& twigRecords = packed.tables.twigs;
    uploadBytes(allocator, sets, packed.sets.data(), packed.sets.size() * sizeof(GpuTwigSet));
    uploadBytes(allocator, bricks, brickRecords.data(), std::max<std::size_t>(brickRecords.size(), 1) * sizeof(GpuTwigBrick));
    uploadBytes(allocator, twigs, twigRecords.data(), std::max<std::size_t>(twigRecords.size(), 1) * sizeof(GpuTwig));
    uploadBytes(allocator, twigBindings, packed.bindings.data(), std::max<std::size_t>(packed.bindings.size(), 1) * sizeof(std::uint32_t));
    twigBindingRecords = std::move(packed.bindings);

    // The far tables, one unused record each without (every binding needs a buffer).
    const auto uploadAtLeastOne = [&](VulkanBuffer& buffer, const auto& records) {
        using Record = typename std::decay_t<decltype(records)>::value_type;
        const Record none{};
        uploadBytes(allocator, buffer, records.empty() ? static_cast<const void*>(&none) : static_cast<const void*>(records.data()),
                    std::max<std::size_t>(records.size(), 1) * sizeof(Record));
    };
    uploadAtLeastOne(farNodes, packed.tables.farNodes);
    uploadAtLeastOne(farLevels, packed.tables.farLevels);
    uploadAtLeastOne(farClusters, packed.tables.farClusters);
    uploadAtLeastOne(farLeaves, packed.tables.farLeaves);
    uploadAtLeastOne(farShapes, packed.tables.farShapes);
    uploadBytes(allocator, prototypes, prototypeRecords.data(), prototypeRecords.size() * sizeof(GpuTwigPrototype));
    uploadBytes(allocator, levels, levelRecords.data(), levelRecords.size() * sizeof(GpuTwigLevel));
    madeFrom = table.twigSets;
    madePlaces = places;
    return true;
}

/*
* The view and groups uploaded, the room written (the sources pass clears the counts), the working buffers grown to the
* room; then the sources pass, the cull pass (indirect: its dispatch the sources pass wrote), and the regions and entries
* made ready for the cluster passes, the mesh shaders and the copy back of the counts.
*/
void VulkanTwigDrawing::record(VkCommandBuffer commands, FrameDescriptors& descriptors, const PlacedInputs& placed, const BoneCullBuffers& bones,
                               const GpuCameraCull& view, bool farLeaves, std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);
    const VmaAllocator allocator = device.getAllocator();
    uploadBytes(allocator, frame.view, &view, sizeof(view));
    std::vector<std::uint32_t> groupWords{static_cast<std::uint32_t>(twigGroups.size())};
    groupWords.insert(groupWords.end(), twigGroups.begin(), twigGroups.end());
    uploadBytes(allocator, frame.groups, groupWords.data(), groupWords.size() * sizeof(std::uint32_t));

    // The room after the counts (the previous use of this frame's regions finished with its fence).
    const std::uint32_t roomWords[4] = {frame.room, 0, 0, 0};
    vkCmdUpdateBuffer(commands, frame.regions.get(), countBytes, sizeof(roomWords), roomWords);
    bufferBarrier(commands, frame.regions.get(), transferWrite, computeReadWrite);

    std::vector<ComputeBinding> bindings{
        storageBufferBinding(frame.view.get()),   storageBufferBinding(placed.commands), storageBufferBinding(placed.entries),
        storageBufferBinding(placed.partInstances.buffer), storageBufferBinding(placed.parts), storageBufferBinding(placed.plants),
        storageBufferBinding(frame.groups.get()), storageBufferBinding(sets.get()),      storageBufferBinding(bricks.get()),
        storageBufferBinding(twigs.get()),        storageBufferBinding(prototypes.get()), storageBufferBinding(levels.get()),
        storageBufferBinding(frame.sources.get()), storageBufferBinding(frame.dispatch.get()), storageBufferBinding(frame.regions.get()),
        storageBufferBinding(frame.entries.get()), storageBufferBinding(bones.plantTrees), storageBufferBinding(bones.trees),
        storageBufferBinding(bones.poses),        storageBufferBinding(bones.twigBindings),
        storageBufferBinding(farNodes.get()),     storageBufferBinding(farLevels.get()),
    };
    bindings[3].buffer = placed.partInstances;
    const std::uint32_t pushed[4] = {mostBricks, mostSources, bones.stamp, farLeaves ? 1u : 0u};

    bindComputeSet(commands, device.getDevice(), descriptors, *sourcesPass, bindings);
    vkCmdPushConstants(commands, sourcesPass->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, pushed);
    vkCmdDispatch(commands, 1, 1, 1);

    bufferBarrier(commands, frame.sources.get(), computeWrite, computeReadWrite);
    bufferBarrier(commands, frame.regions.get(), computeReadWrite, computeReadWrite);
    bufferBarrier(commands, frame.dispatch.get(), computeWrite, indirectRead);

    bindComputeSet(commands, device.getDevice(), descriptors, *cullPass, bindings);
    vkCmdPushConstants(commands, cullPass->getLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, pushBytes, pushed);
    vkCmdDispatchIndirect(commands, frame.dispatch.get(), 0);

    const BufferUse then = drawingRead(device);
    bufferBarrier(commands, frame.regions.get(), computeReadWrite, then);
    bufferBarrier(commands, frame.entries.get(), computeWrite, then);

    // The regions' counts, then the tally (past the room) straight after them.
    const VkBufferCopy copies[2] = {{0, 0, readCountBytes}, {regionsTallyOffset, readCountBytes, tallyBytes}};
    vkCmdCopyBuffer(commands, frame.regions.get(), frame.readback.get(), 2, copies);
    frame.pending = true;
}

VkBuffer VulkanTwigDrawing::getRegions(std::uint32_t frameIndex) const {
    return frames.at(frameIndex).regions.get();
}

VkBuffer VulkanTwigDrawing::getEntries(std::uint32_t frameIndex) const {
    return frames.at(frameIndex).entries.get();
}

VkBuffer VulkanTwigDrawing::getTwigs() const {
    return twigs.get();
}

VkBuffer VulkanTwigDrawing::getFarClusters() const {
    return farClusters.get();
}

VkBuffer VulkanTwigDrawing::getFarLeaves() const {
    return farLeaves.get();
}

VkBuffer VulkanTwigDrawing::getFarShapes() const {
    return farShapes.get();
}

VkBuffer VulkanTwigDrawing::getTwigBindings() const {
    return twigBindings.get();
}

std::span<const std::uint32_t> VulkanTwigDrawing::getTwigBindingRecords() const {
    return twigBindingRecords;
}

std::uint32_t VulkanTwigDrawing::getRoom(std::uint32_t frameIndex) const {
    return frames.at(frameIndex).room;
}

// A region past its room grows every region's room to the next power of two that holds it, up to largestRoom.
std::optional<VulkanTwigDrawing::Collected> VulkanTwigDrawing::collect(std::uint32_t frameIndex) {
    PerFrame& frame = frames.at(frameIndex);

    if (!frame.pending) {
        return std::nullopt;
    }

    frame.pending = false;
    frame.readback.invalidate();
    Collected collected;
    const auto* mapped = static_cast<const std::uint8_t*>(frame.readback.getMapped());
    std::memcpy(collected.counts.data(), mapped, readCountBytes);
    std::array<std::uint32_t, TWIG_TALLY_WORDS> tally{};
    std::memcpy(tally.data(), mapped + readCountBytes, tallyBytes);
    collected.tally = readTwigTally(tally);
    const std::uint32_t most = *std::max_element(collected.counts.begin(), collected.counts.end());

    if (most > room) {
        room = std::min(std::bit_ceil(most), largestRoom);
    }

    return collected;
}
