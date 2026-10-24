#include "core/job_system.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
    // Every index in 0..count-1 runs exactly once, whatever the split and however many threads.
    void checkEveryIndexOnce(JobSystem& jobs, std::size_t count, std::size_t grain) {
        std::vector<std::atomic<int>> runs(count);
        jobs.parallelFor(count, grain, [&](std::size_t index) {
            runs[index].fetch_add(1, std::memory_order_relaxed);
        });

        for (const std::atomic<int>& run : runs) {
            assert(run.load() == 1);
        }
    }

    // A sum whose rounding depends on the order of its additions: terms of very different sizes.
    float term(std::size_t index) {
        return (index % 3 == 0 ? 1.0e6f : 1.0e-3f) * (1.0f + static_cast<float>(index % 17) / 7.0f);
    }

    /*
    * The deterministic way to sum in parallel: fixed chunks, each summed on its own in index order, then the chunks'
    * totals added in chunk order. Which thread sums which chunk does not matter, so the result is the same to the last
    * bit on any number of threads.
    */
    float chunkedSum(JobSystem& jobs, std::size_t count, std::size_t chunkSize) {
        const std::size_t chunks = (count + chunkSize - 1) / chunkSize;
        std::vector<float> partial(chunks, 0.0f);
        jobs.parallelFor(chunks, 1, [&](std::size_t chunk) {
            const std::size_t end = std::min(count, (chunk + 1) * chunkSize);
            float sum = 0.0f;
            for (std::size_t index = chunk * chunkSize; index < end; ++index) {
                sum += term(index);
            }
            partial[chunk] = sum;
        });

        float total = 0.0f;
        for (float sum : partial) {
            total += sum;
        }
        return total;
    }

    bool sameBits(float a, float b) {
        std::uint32_t aBits = 0;
        std::uint32_t bBits = 0;
        std::memcpy(&aBits, &a, sizeof(float));
        std::memcpy(&bBits, &b, sizeof(float));
        return aBits == bBits;
    }
}

void testJobSystem() {
    // With no workers, the calling thread is the only one, and parallelFor still runs every index.
    {
        JobSystem jobs{0};
        assert(jobs.getThreadCount() == 1);
        checkEveryIndexOnce(jobs, 1000, 16);
    }

    // The default leaves one core for the calling thread itself.
    {
        const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
        assert(defaultWorkerCount() == cores - 1);
    }

    // Every index once: no indices, one, fewer than the grain, a count the grain does not divide, and many.
    {
        JobSystem jobs{4};
        assert(jobs.getThreadCount() == 5);
        checkEveryIndexOnce(jobs, 0, 1);
        checkEveryIndexOnce(jobs, 1, 1);
        checkEveryIndexOnce(jobs, 5, 64);
        checkEveryIndexOnce(jobs, 1001, 7);
        checkEveryIndexOnce(jobs, 100000, 1);
        checkEveryIndexOnce(jobs, 100000, 256);
    }

    // The same results on one thread and on many: each index writes only its own slot.
    {
        JobSystem serial{0};
        JobSystem parallel{7};
        std::vector<float> one(5000);
        std::vector<float> many(5000);
        const auto fill = [](std::vector<float>& values) {
            return [&values](std::size_t index) {
                values[index] = term(index) * 0.5f + static_cast<float>(index);
            };
        };
        serial.parallelFor(one.size(), 8, fill(one));
        parallel.parallelFor(many.size(), 8, fill(many));
        assert(std::memcmp(one.data(), many.data(), one.size() * sizeof(float)) == 0);
    }

    // A parallel sum by fixed chunks gives the same bits on any number of threads, every time.
    {
        JobSystem serial{0};
        JobSystem parallel{7};
        const float expected = chunkedSum(serial, 200000, 1024);
        for (int run = 0; run < 20; ++run) {
            assert(sameBits(chunkedSum(parallel, 200000, 1024), expected));
        }
    }

    // A parallelFor inside a job finishes: the waiting threads run jobs instead of sleeping, so nobody waits forever.
    {
        JobSystem jobs{4};
        std::vector<std::atomic<int>> runs(16 * 1000);
        jobs.parallelFor(16, 1, [&](std::size_t outer) {
            jobs.parallelFor(1000, 10, [&](std::size_t inner) {
                runs[outer * 1000 + inner].fetch_add(1, std::memory_order_relaxed);
            });
        });
        for (const std::atomic<int>& run : runs) {
            assert(run.load() == 1);
        }
    }

    // An exception thrown by a job reaches the caller once everything has stopped, and the system still works after.
    {
        JobSystem jobs{4};
        bool caught = false;
        try {
            jobs.parallelFor(10000, 4, [](std::size_t index) {
                if (index == 5000) {
                    throw std::runtime_error("job failed");
                }
            });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        assert(caught);
        checkEveryIndexOnce(jobs, 10000, 4);
    }

    // The same with no workers: the exception comes straight through.
    {
        JobSystem jobs{0};
        bool caught = false;
        try {
            jobs.parallelFor(10, 1, [](std::size_t index) {
                if (index == 3) {
                    throw std::runtime_error("job failed");
                }
            });
        } catch (const std::runtime_error&) {
            caught = true;
        }
        assert(caught);
    }

    // A grain of 0 would split forever.
    {
        JobSystem jobs{2};
        bool threw = false;
        try {
            jobs.parallelFor(10, 0, [](std::size_t) {});
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    // The work really spreads: slow indices are run by more than one thread.
    {
        JobSystem jobs{4};
        std::mutex mutex;
        std::set<std::thread::id> threads;
        jobs.parallelFor(64, 1, [&](std::size_t) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            const std::lock_guard<std::mutex> lock{mutex};
            threads.insert(std::this_thread::get_id());
        });
        assert(threads.size() > 1);
    }

    // Two threads outside the system may use it at once.
    {
        JobSystem jobs{3};
        std::vector<std::atomic<int>> runs(2 * 20000);
        const auto half = [&](std::size_t offset) {
            jobs.parallelFor(20000, 16, [&, offset](std::size_t index) {
                runs[offset + index].fetch_add(1, std::memory_order_relaxed);
            });
        };
        std::thread other{half, 20000};
        half(0);
        other.join();
        for (const std::atomic<int>& run : runs) {
            assert(run.load() == 1);
        }
    }
}
