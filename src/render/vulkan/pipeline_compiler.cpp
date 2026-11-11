#include "render/vulkan/pipeline_compiler.h"

#include <algorithm>
#include <utility>

PipelineFuture::PipelineFuture(PipelineCompiler& compiler, std::shared_ptr<PendingPipeline> pending):
    compiler(&compiler),
    pending(std::move(pending))
{
}

PipelineFuture::~PipelineFuture() {
    abandon();
}

PipelineFuture::PipelineFuture(PipelineFuture&& other) noexcept:
    compiler(std::exchange(other.compiler, nullptr)),
    pending(std::move(other.pending))
{
}

PipelineFuture& PipelineFuture::operator=(PipelineFuture&& other) noexcept {
    if (this != &other) {
        abandon();
        compiler = std::exchange(other.compiler, nullptr);
        pending = std::move(other.pending);
    }

    return *this;
}

VkPipeline PipelineFuture::take() {
    if (!pending) {
        return VK_NULL_HANDLE;
    }

    const std::shared_ptr<PendingPipeline> taking = std::move(pending);
    return compiler->take(*taking);
}

bool PipelineFuture::valid() const {
    return pending != nullptr;
}

void PipelineFuture::abandon() noexcept {
    if (pending) {
        compiler->abandon(*pending);
        pending.reset();
    }
}

PipelineCompiler::PipelineCompiler(unsigned workers, Destroy destroy):
    destroy(std::move(destroy))
{
    const unsigned count = std::max(workers, 1u);
    threads.reserve(count);

    for (unsigned thread = 0; thread < count; ++thread) {
        threads.emplace_back([this] { runWorker(); });
    }
}

// The threads run the whole queue before they stop, so every build asked for finishes.
PipelineCompiler::~PipelineCompiler() {
    {
        const std::lock_guard<std::mutex> guard{lock};
        stopping = true;
    }

    work.notify_all();

    for (std::thread& thread : threads) {
        thread.join();
    }
}

PipelineFuture PipelineCompiler::submit(std::function<VkPipeline()> build) {
    auto pending = std::make_shared<PendingPipeline>();
    pending->build = std::move(build);

    {
        const std::lock_guard<std::mutex> guard{lock};
        queue.push_back(pending);
        ++total;
    }

    work.notify_one();
    return PipelineFuture{*this, std::move(pending)};
}

PipelineProgress PipelineCompiler::progress() const {
    const std::lock_guard<std::mutex> guard{lock};
    return PipelineProgress{built, total};
}

void PipelineCompiler::waitAll() {
    std::unique_lock<std::mutex> guard{lock};
    finished.wait(guard, [this] { return built >= total; });

    // A failure take has not already handed on (which clears it), once.
    if (firstUnreported) {
        std::exception_ptr error = std::exchange(firstUnreported, nullptr);
        guard.unlock();
        std::rethrow_exception(error);
    }
}

unsigned PipelineCompiler::defaultWorkers() {
    return std::max(std::thread::hardware_concurrency(), 1u);
}

/*
* Each thread takes the oldest queued build, runs it unlocked, and records what came of it: a pipeline nobody will
* take is destroyed at once; a failure is also kept for waitAll, the first one nobody has heard of yet. A build whose
* owner went before it started is skipped.
*/
void PipelineCompiler::runWorker() {
    while (true) {
        std::shared_ptr<PendingPipeline> pending;

        {
            std::unique_lock<std::mutex> guard{lock};
            work.wait(guard, [this] { return stopping || !queue.empty(); });

            if (queue.empty()) {
                return;
            }

            pending = std::move(queue.front());
            queue.pop_front();

            // Nobody will take it (its owner went before it started, as when the window closes while loading): done
            // without building it.
            if (pending->abandoned) {
                pending->build = nullptr;
                pending->finished = true;
                ++built;
                guard.unlock();
                finished.notify_all();
                continue;
            }
        }

        VkPipeline result = VK_NULL_HANDLE;
        std::exception_ptr error;

        try {
            result = pending->build();
        } catch (...) {
            error = std::current_exception();
        }

        VkPipeline unwanted = VK_NULL_HANDLE;

        {
            const std::lock_guard<std::mutex> guard{lock};
            pending->build = nullptr;
            pending->result = result;
            pending->error = error;
            pending->finished = true;

            if (pending->abandoned) {
                unwanted = std::exchange(pending->result, VK_NULL_HANDLE);
            }

            if (error && !firstUnreported) {
                firstUnreported = error;
            }

            ++built;
        }

        if (unwanted != VK_NULL_HANDLE) {
            destroy(unwanted);
        }

        finished.notify_all();
    }
}

// Waits for this build, then hands over its pipeline or rethrows its error.
VkPipeline PipelineCompiler::take(PendingPipeline& pending) {
    std::unique_lock<std::mutex> guard{lock};
    finished.wait(guard, [&pending] { return pending.finished; });

    if (pending.error) {
        // Told here, so waitAll need not tell it again.
        if (firstUnreported == pending.error) {
            firstUnreported = nullptr;
        }

        std::rethrow_exception(pending.error);
    }

    return std::exchange(pending.result, VK_NULL_HANDLE);
}

// A finished pipeline nobody took is destroyed now; one still building, when it is built (runWorker).
void PipelineCompiler::abandon(PendingPipeline& pending) noexcept {
    VkPipeline unwanted = VK_NULL_HANDLE;

    {
        const std::lock_guard<std::mutex> guard{lock};
        pending.abandoned = true;

        if (pending.finished) {
            unwanted = std::exchange(pending.result, VK_NULL_HANDLE);
        }
    }

    if (unwanted != VK_NULL_HANDLE) {
        destroy(unwanted);
    }
}
