#include "render/vulkan/vulkan_sky_visibility.h"

#include "core/profiler.h"
#include "render/vulkan/image_state.h"
#include "render/vulkan/immediate_commands.h"

#include <SDL3/SDL_thread.h>
#include <algorithm>
#include <cstring>
#include <thread>

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
* from its first frame. A frame bakes at least a round of jobs for every core, and only such batches measure the cost:
* a smaller one runs on fewer cores and would measure a point dearer than it is (skyCellsForBudget).
*
* With backgroundBake, the frame only hands batches to the background baker and applies what it finished: a batch of
* about skyBackgroundBatchSeconds, the waiting points first (quickly, while the quick rays are fewer), then refinement,
* then the deep rays. On the frame, the deep rays take what budget is left once nothing waits or is refined; a capture
* gives every point its deep rays too.
* A fresh grid and a capture still bake whole on the frame, after waiting for the batch baking (so nothing it brings
* lands after them).
*/
void VulkanSkyVisibility::bake(const FrameDescription& frame, const Vec3& centre, const SkyVisibilitySettings& settings, JobSystem& jobs,
                               bool everything, const std::optional<Vec3>& toSun) {
    const bool afresh = grid.follow(centre, settings, frame.staticLevelVersion);
    grid.followSun(toSun);

    // The sunlit spots the sun's bounce aims at, found again only when the sun or the grid has moved on.
    if (frame.staticLevel != nullptr) {
        grid.prepareSunEmitters(*frame.staticLevel, frame.staticAlbedo ? std::span<const Vec3>{*frame.staticAlbedo} : std::span<const Vec3>{}, jobs);
    }
    const double quickShare = static_cast<double>(settings.quickRays) / static_cast<double>(settings.rays);
    const double deepShare = static_cast<double>(settings.deepRays) / static_cast<double>(settings.rays);
    const int minimumBatch = skyMinimumBatch(jobs.getThreadCount());
    applyFinished(settings.rays, minimumBatch);

    if (grid.getWaiting() + grid.getRefining() + grid.getDeepening() == 0 || frame.staticLevel == nullptr) {
        return;
    }

    if (settings.backgroundBake && !everything && !afresh) {
        startBackground(frame, settings);
        return;
    }

    if (baker && baker->busy()) {
        baker->waitFinished();
        applyFinished(settings.rays, minimumBatch);
    }

    const int waiting = grid.getWaiting();
    const int refining = grid.getRefining();
    const double budgetSeconds = settings.bakeMilliseconds * 1e-3;
    const int budget = skyCellsForBudget(budgetSeconds, secondsPerCell, waiting + refining, minimumBatch);
    const int quickBudget = skyCellsForBudget(budgetSeconds, secondsPerCell * quickShare, waiting, minimumBatch);
    const SkyBakePlan plan = planSkyBake(waiting, refining, afresh, everything, budget, quickBudget, settings.quickRays < settings.rays);
    const int count = plan.waiting + plan.refine;

    const std::span<const Vec3> albedo = frame.staticAlbedo ? std::span<const Vec3>{*frame.staticAlbedo} : std::span<const Vec3>{};

    if (count > 0) {
        const double start = profilerSeconds();
        const int baked = grid.bake(*frame.staticLevel, jobs, plan.waiting, plan.quick, frame.camera.getForward(), albedo);
        // A capture also refines what this very bake left unfinished (points baked before the grid round their hits was).
        const int refined = grid.refine(*frame.staticLevel, jobs, everything ? grid.getRefining() : plan.refine, albedo);

        if (baked + refined >= minimumBatch) {
            secondsPerCell = (profilerSeconds() - start) / (plan.quick ? quickShare : 1.0) / static_cast<double>(baked + refined);
        }
    }

    // The deep rays last: a capture's every point (those this very bake queued too), else what the budget leaves.
    const int deepening = grid.getDeepening();

    if (deepening > 0 && (everything || (count == 0 && !afresh))) {
        const int deep = everything ? deepening : skyCellsForBudget(budgetSeconds, secondsPerCell * deepShare, deepening, minimumBatch);
        const double start = profilerSeconds();
        const int baked = grid.deepen(*frame.staticLevel, jobs, deep, albedo);

        if (baked >= minimumBatch) {
            secondsPerCell = skySecondsAtFullRays((profilerSeconds() - start) / static_cast<double>(baked), settings.deepRays, settings.rays);
        }
    }
}

// A finished background batch into the grid, its cost (at the full rays) kept when it was big enough to measure it.
void VulkanSkyVisibility::applyFinished(int fullRays, int minimumBatch) {
    if (!baker) {
        return;
    }

    const std::optional<SkyBakeBatch> done = baker->takeFinished();

    if (!done) {
        return;
    }

    if (static_cast<int>(done->entries.size()) >= minimumBatch && done->secondsPerCell > 0.0) {
        secondsPerCell = skySecondsAtFullRays(done->secondsPerCell, done->rays, fullRays);
    }

    grid.applyBatch(*done);
}

// The next batch for the background baker, when it is free: the waiting points, else refinement, else the deep rays.
void VulkanSkyVisibility::startBackground(const FrameDescription& frame, const SkyVisibilitySettings& settings) {
    // Its threads, the baker's own among them: the settings' share of the CPU's. Remade when that changes, once the
    // batch baking is in.
    const unsigned threads = skyBackgroundWorkers(settings.backgroundShare, std::max(std::thread::hardware_concurrency(), 1u));

    if (baker && bakeJobs->getThreadCount() != threads) {
        baker->waitFinished();
        applyFinished(settings.rays, skyMinimumBatch(bakeJobs->getThreadCount()));
        baker.reset();
        bakeJobs.reset();
    }

    if (!baker) {
        // Below the frame's threads, so the frame comes first and the bake takes what is left.
        const auto lowPriority = [] {
            SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);
        };
        bakeJobs = std::make_unique<JobSystem>(threads - 1, lowPriority);
        baker = std::make_unique<SkyBackgroundBaker>(*bakeJobs, lowPriority);
    }

    if (baker->busy()) {
        return;
    }

    const int minimumBatch = skyMinimumBatch(bakeJobs->getThreadCount());
    const int waiting = grid.getWaiting();
    const int refining = grid.getRefining();
    const bool quick = waiting > 0 && settings.quickRays < settings.rays;
    const int rays = quick ? settings.quickRays : waiting > 0 || refining > 0 ? settings.rays : settings.deepRays;
    const double seconds = secondsPerCell * static_cast<double>(rays) / static_cast<double>(settings.rays);
    const int cells = skyCellsForBudget(skyBackgroundBatchSeconds, seconds, waiting > 0 ? waiting : refining > 0 ? refining : grid.getDeepening(), minimumBatch);
    SkyBakeBatch batch = waiting > 0   ? grid.takeBatch(cells, quick, frame.camera.getForward())
                         : refining > 0 ? grid.takeRefineBatch(cells)
                                        : grid.takeDeepBatch(cells);
    baker->start(std::move(batch), frame.staticLevel, frame.staticAlbedo);
}

/*
* The copy goes through this frame in flight's own staging buffer, which the GPU has finished with (its frame came
* round again), and its barrier waits for every earlier frame's reads of the image. Only the points changed since the
* last copy are packed again (packing all of them, half floats and all, cost milliseconds every frame while baking);
* the packed copy is whole, so each staging buffer gets all of it.
*/
void VulkanSkyVisibility::record(VkCommandBuffer commands, std::uint32_t frameIndex) {
    if (grid.getVersion() == uploadedVersion) {
        return;
    }

    const std::span<const SkyCell> baked = grid.getCells();
    const bool everyPoint = grid.takeChanges(changed) || packed.size() != baked.size();

    if (everyPoint) {
        packed.resize(baked.size());

        for (std::size_t cell = 0; cell < baked.size(); ++cell) {
            packed[cell] = packSkyCell(baked[cell]);
        }
    } else {
        for (const int cell : changed) {
            packed[static_cast<std::size_t>(cell)] = packSkyCell(baked[static_cast<std::size_t>(cell)]);
        }
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
