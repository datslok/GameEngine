#pragma once

#include "volk/volk.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

/*
* Pipelines built on worker threads. The driver compiles each pipeline's shaders into the GPU's own code when the
* pipeline is made, which after a build that changed them takes minutes for all of them one after another (measured on
* the laptop, RTX 4070 Laptop: 199.5 s to start with an empty driver cache, about 5 s with a full one). Making pipelines
* is safe from many threads at once (Vulkan asks only that one object is not changed from two threads together), so the
* pipeline classes (render/vulkan/vulkan_pipeline.h, vulkan_compute.h) hand the slow part here and carry on; whoever
* first uses a pipeline waits for it if it is not built yet, so nothing else has to know, and the application shows how
* many are built (render/gpu/loading_screen.h) while it waits for the rest.
*
* Holds nothing of the GPU itself: what a build makes and how to destroy one nobody takes are given to it, so the tests
* run it without one.
*/

// How many pipelines were asked for and how many are built (or failed).
struct PipelineProgress {
    std::size_t built = 0;
    std::size_t total = 0;

    bool done() const {
        return built >= total;
    }
};

class PipelineCompiler;

// One build's result: what both sides share. Kept by the compiler's queue and the future until both are finished.
struct PendingPipeline {
    std::function<VkPipeline()> build;
    VkPipeline result = VK_NULL_HANDLE;
    std::exception_ptr error;
    bool finished = false;
    bool abandoned = false; // the future went without taking it: the compiler destroys it once built
};

/*
* A pipeline being built: take() waits for it and hands it over (or rethrows its build's error), once. Going without
* taking it gives it back to the compiler, which destroys it once built. Move-only.
*/
class PipelineFuture {
public:
    PipelineFuture() = default;
    PipelineFuture(PipelineCompiler& compiler, std::shared_ptr<PendingPipeline> pending);
    ~PipelineFuture();

    PipelineFuture(const PipelineFuture&) = delete;
    PipelineFuture& operator=(const PipelineFuture&) = delete;
    PipelineFuture(PipelineFuture&& other) noexcept;
    PipelineFuture& operator=(PipelineFuture&& other) noexcept;

    VkPipeline take();
    bool valid() const;

private:
    PipelineCompiler* compiler = nullptr;
    std::shared_ptr<PendingPipeline> pending;

    void abandon() noexcept;
};

class PipelineCompiler {
public:
    using Destroy = std::function<void(VkPipeline)>;

    // workers threads (at least one), started at once and waiting for builds; destroy for pipelines nobody takes.
    PipelineCompiler(unsigned workers, Destroy destroy);

    // Finishes every build asked for (those nobody will take are destroyed) and stops the threads.
    ~PipelineCompiler();

    PipelineCompiler(const PipelineCompiler&) = delete;
    PipelineCompiler& operator=(const PipelineCompiler&) = delete;

    // Queues a build, run on the first free thread in the order asked.
    PipelineFuture submit(std::function<VkPipeline()> build);

    PipelineProgress progress() const;

    // Waits for every build asked for so far; then rethrows the first error nobody was told of yet.
    void waitAll();

    // A thread count for the machine: one per core it reports, at least one.
    static unsigned defaultWorkers();

private:
    friend class PipelineFuture;

    mutable std::mutex lock;
    std::condition_variable work;     // a build was queued, or the threads should stop
    std::condition_variable finished; // a build finished
    std::deque<std::shared_ptr<PendingPipeline>> queue;
    std::size_t built = 0;
    std::size_t total = 0;
    bool stopping = false;

    // The first build error neither take nor waitAll has handed on yet.
    std::exception_ptr firstUnreported;
    Destroy destroy;
    std::vector<std::thread> threads;

    void runWorker();
    VkPipeline take(PendingPipeline& pending);
    void abandon(PendingPipeline& pending) noexcept;
};
