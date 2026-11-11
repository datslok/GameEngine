#include "render/vulkan/vulkan_sky_visibility.h"

#include "core/profiler.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/immediate_commands.h"

#include <algorithm>
#include <cstring>

namespace {
    // A point is three texels of four 32-bit words each along x (GpuSkyCell, skyCellTexels), read by exact texels.
    constexpr VkFormat cellFormat = VK_FORMAT_R32G32B32A32_UINT;

    constexpr std::uint32_t across = SKY_VISIBILITY_ACROSS;
    constexpr std::uint32_t high = SKY_VISIBILITY_HIGH;
    constexpr std::uint32_t mostCells = across * high * across * SKY_VISIBILITY_MAX_CASCADES;
}

/*
* The image has room for the most cascades from the start (10.6 MB), so changing their number never remakes it, and is
* cleared to zero (every point "not baked") and left readable, so a frame that reads it before any bake sees open sky.
*/
VulkanSkyVisibility::VulkanSkyVisibility(const VulkanDevice& device, ImmediateCommands& uploads, VkSampler nearest):
    device(device),
    nearest(nearest)
{
    ImageDescription description;
    description.format = cellFormat;
    description.width = across * skyCellTexels;
    description.height = high;
    description.depth = across * SKY_VISIBILITY_MAX_CASCADES;
    description.volume = true;
    description.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    description.name = "Sky visibility";
    cells = VulkanImage{device, description};

    uploads.run([&](VkCommandBuffer commands) {
        cells.transition(commands, ImageState::TransferDestination, ImageContents::Discard);
        const VkClearColorValue unknown{};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = 1;
        range.layerCount = 1;
        vkCmdClearColorImage(commands, cells.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &unknown, 1, &range);
        cells.transition(commands, ImageState::SurfaceRead);
    });
}

/*
* The budget is CPU time on every core, from what a point cost at full rays last time (planSkyBake says what a frame
* bakes). A quick bake's cost is scaled up by the rays' ratio, so the refinement after a refill keeps to the budget
* from its first frame.
*/
void VulkanSkyVisibility::bake(const FrameDescription& frame, const Vec3& centre, const SkyVisibilitySettings& settings, JobSystem& jobs,
                               bool everything) {
    const bool afresh = grid.follow(centre, settings, frame.staticLevelVersion);
    const int waiting = grid.getWaiting();
    const int refining = grid.getRefining();

    if (waiting + refining == 0 || frame.staticLevel == nullptr) {
        return;
    }

    const double budgetSeconds = settings.bakeMilliseconds * 1e-3;
    const double quickShare = static_cast<double>(settings.quickRays) / static_cast<double>(settings.rays);
    const int budget = skyCellsForBudget(budgetSeconds, secondsPerCell, waiting + refining);
    const int quickBudget = skyCellsForBudget(budgetSeconds, secondsPerCell * quickShare, waiting);
    const SkyBakePlan plan = planSkyBake(waiting, refining, afresh, everything, budget, quickBudget, settings.quickRays < settings.rays);
    const int count = plan.waiting + plan.refine;

    if (count > 0) {
        const double start = profilerSeconds();
        const std::span<const Vec3> albedo = frame.staticAlbedo ? std::span<const Vec3>{*frame.staticAlbedo} : std::span<const Vec3>{};
        grid.bake(*frame.staticLevel, jobs, plan.waiting, plan.quick, frame.camera.getForward(), albedo);
        // A capture also refines what this very bake left unfinished (points baked before the grid round their hits was).
        const int refined = grid.refine(*frame.staticLevel, jobs, everything ? grid.getRefining() : plan.refine, albedo);
        secondsPerCell = (profilerSeconds() - start) / (plan.quick ? quickShare : 1.0) / static_cast<double>(std::max(plan.waiting + refined, 1));
    }
}

/*
* The copy goes through this frame in flight's own staging buffer, which the GPU has finished with (its frame came
* round again), and its barrier waits for every earlier frame's reads of the image.
*/
void VulkanSkyVisibility::record(VkCommandBuffer commands, std::uint32_t frameIndex) {
    if (grid.getVersion() == uploadedVersion) {
        return;
    }

    const std::span<const SkyCell> baked = grid.getCells();
    packed.resize(baked.size());

    for (std::size_t cell = 0; cell < baked.size(); ++cell) {
        packed[cell] = packSkyCell(baked[cell]);
    }

    VulkanBuffer& buffer = staging[frameIndex];

    if (buffer.get() == VK_NULL_HANDLE) {
        buffer = VulkanBuffer{device.getAllocator(), mostCells * sizeof(GpuSkyCell), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, BufferMemory::Upload};
    }

    std::memcpy(buffer.getMapped(), packed.data(), packed.size() * sizeof(GpuSkyCell));
    buffer.flush();

    VkBufferImageCopy copy{};
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {across * skyCellTexels, high, static_cast<std::uint32_t>(packed.size() / (across * high))};

    cells.transition(commands, ImageState::TransferDestination, ImageContents::Discard);
    vkCmdCopyBufferToImage(commands, buffer.get(), cells.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    cells.transition(commands, ImageState::SurfaceRead);
    uploadedVersion = grid.getVersion();
}

std::span<const ProbeCascade> VulkanSkyVisibility::getCascades() const {
    return grid.getCascades();
}

std::string VulkanSkyVisibility::describePoints(const Vec3& place) const {
    return grid.describePoints(place);
}

SampledImage VulkanSkyVisibility::sampled() const {
    return SampledImage{cells.getView(), nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
}
