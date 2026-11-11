#include "render/vulkan/pipeline_compiler.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
    // A stand-in handle: no GPU is involved, only the queue.
    VkPipeline fakePipeline(std::uintptr_t number) {
        return reinterpret_cast<VkPipeline>(number);
    }

    // The handles the compiler destroyed itself, in no particular order.
    struct Destroyed {
        std::mutex lock;
        std::vector<VkPipeline> handles;

        PipelineCompiler::Destroy callback() {
            return [this](VkPipeline pipeline) {
                const std::lock_guard<std::mutex> guard{lock};
                handles.push_back(pipeline);
            };
        }

        bool has(VkPipeline pipeline) {
            const std::lock_guard<std::mutex> guard{lock};
            for (const VkPipeline handle : handles) {
                if (handle == pipeline) {
                    return true;
                }
            }
            return false;
        }

        std::size_t count() {
            const std::lock_guard<std::mutex> guard{lock};
            return handles.size();
        }
    };

    // Waits up to a few seconds for a condition another thread makes true.
    template <typename Condition>
    bool eventually(Condition condition) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (condition()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return condition();
    }

    // Four builds on four workers all run at once: each waits until all four have started.
    void testBuildsRunTogether() {
        Destroyed destroyed;
        PipelineCompiler compiler{4, destroyed.callback()};
        std::atomic<int> started{0};
        std::vector<PipelineFuture> futures;

        for (std::uintptr_t number = 1; number <= 4; ++number) {
            futures.push_back(compiler.submit([&started, number] {
                ++started;
                eventually([&started] { return started.load() == 4; });
                return fakePipeline(number);
            }));
        }

        for (std::uintptr_t number = 1; number <= 4; ++number) {
            assert(futures[number - 1].take() == fakePipeline(number));
        }

        assert(started.load() == 4);
        assert(destroyed.count() == 0);
    }

    // Progress counts what was asked for and what is done; waiting for all finishes every build.
    void testProgress() {
        Destroyed destroyed;
        PipelineCompiler compiler{2, destroyed.callback()};
        assert(compiler.progress().built == 0 && compiler.progress().total == 0);
        std::vector<PipelineFuture> futures;

        for (std::uintptr_t number = 1; number <= 10; ++number) {
            futures.push_back(compiler.submit([number] {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return fakePipeline(number);
            }));
        }

        assert(compiler.progress().total == 10);
        compiler.waitAll();
        assert(compiler.progress().built == 10 && compiler.progress().done());

        for (PipelineFuture& future : futures) {
            assert(future.take() != VK_NULL_HANDLE);
        }
    }

    // A failed build's error comes back where the pipeline is taken, and from waiting for all, once.
    void testErrorsComeBack() {
        Destroyed destroyed;
        PipelineCompiler compiler{2, destroyed.callback()};
        PipelineFuture failing = compiler.submit([]() -> VkPipeline { throw std::runtime_error("bad shader"); });
        PipelineFuture fine = compiler.submit([] { return fakePipeline(7); });

        bool threw = false;
        try {
            compiler.waitAll();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
        compiler.waitAll(); // reported once

        threw = false;
        try {
            failing.take();
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
        assert(fine.take() == fakePipeline(7));
    }

    /*
    * A pipeline nobody takes is the compiler's to destroy: at once when its future goes after it was built, or when it
    * is built when its future went while it was building. Taken ones are the taker's. One whose future went before it
    * started is never built (closing the window while loading skips the rest).
    */
    void testAbandonedPipelinesAreDestroyed() {
        Destroyed destroyed;
        {
            PipelineCompiler compiler{1, destroyed.callback()};
            std::atomic<bool> release{false};
            std::atomic<bool> started{false};
            std::atomic<bool> skippedRan{false};

            PipelineFuture taken = compiler.submit([] { return fakePipeline(1); });
            assert(taken.take() == fakePipeline(1));

            {
                PipelineFuture built = compiler.submit([] { return fakePipeline(2); });
                compiler.waitAll();
            }
            assert(destroyed.has(fakePipeline(2)));

            {
                PipelineFuture early = compiler.submit([&release, &started] {
                    started = true;
                    eventually([&release] { return release.load(); });
                    return fakePipeline(3);
                });

                // Queued behind it on the one thread, so not started when its future goes.
                PipelineFuture skipped = compiler.submit([&skippedRan] {
                    skippedRan = true;
                    return fakePipeline(4);
                });

                assert(eventually([&started] { return started.load(); }));
            }
            assert(!destroyed.has(fakePipeline(3)));
            release = true;
            compiler.waitAll();
            assert(destroyed.has(fakePipeline(3)));
            assert(!skippedRan.load() && compiler.progress().done());
        }

        assert(!destroyed.has(fakePipeline(1)) && !destroyed.has(fakePipeline(4)));
        assert(destroyed.count() == 2);
    }

    // The compiler's end waits for builds still running, and destroys what nobody will take.
    void testTheEndWaits() {
        Destroyed destroyed;
        std::atomic<bool> started{false};
        std::atomic<bool> finished{false};
        {
            PipelineCompiler compiler{1, destroyed.callback()};
            PipelineFuture slow = compiler.submit([&started, &finished] {
                started = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                finished = true;
                return fakePipeline(9);
            });
            PipelineFuture moved = std::move(slow);
            assert(!slow.valid() && moved.valid());
            assert(eventually([&started] { return started.load(); }));
        }
        assert(finished.load());
        assert(destroyed.has(fakePipeline(9)));
    }
}

void testPipelineCompiler() {
    testBuildsRunTogether();
    testProgress();
    testErrorsComeBack();
    testAbandonedPipelinesAreDestroyed();
    testTheEndWaits();
}
