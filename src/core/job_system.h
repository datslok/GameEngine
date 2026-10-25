#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

// One worker per core but one, since the thread that asks for work runs jobs too while it waits.
unsigned defaultWorkerCount();

/*
* How long a worker with nothing to do keeps looking for work before it sleeps. Waking a sleeping thread takes tens of
* microseconds, so work that comes in quick rounds (the cloth's colours, each a few microseconds, many times a tick)
* would spend more time waking workers than working; a short spin keeps them at hand, then they sleep and use nothing.
* Measured on the cloth lab's big sheet (16 threads): 9.3 ms a tick with it, 13.8 without.
*/
inline constexpr double workerSpinSeconds = 50e-6;

/*
* A fixed team of worker threads that run pieces of work ("jobs") on every core, kept alive for the whole run, since
* starting a thread costs far more than handing a waiting one a job.
*
* Work stealing: each thread has its own pile of jobs. It takes its newest job (whose data is likely still in its
* core's cache), and a thread with nothing to do steals the oldest job of another (usually the biggest piece left). A
* thread waiting for its jobs runs jobs meanwhile rather than sleeping, so a parallelFor inside a job cannot leave every
* thread waiting and none working.
*
* Results are deterministic when each index writes only its own output: which thread runs which index, and when,
* varies from run to run. A sum must add fixed chunks in a fixed order (floating-point addition is not associative).
*
* The thread that built the system, and any other thread outside it, shares pile 0; the piles are locked, so several
* such threads may call parallelFor at once.
*/
class JobSystem;

// One shared system with no workers, for code that has no engine at hand (tests, a game run headless): everything runs
// on the caller.
JobSystem& serialJobs();

class JobSystem {
public:
    explicit JobSystem(unsigned workerCount = defaultWorkerCount());
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // The workers plus the calling thread.
    unsigned getThreadCount() const;

    /*
    * Run work(index) for every index in 0..count-1 on any threads, and return when all have finished. The range is
    * halved until a piece holds at most grain indices, each piece run by one thread; a grain big enough that a piece
    * takes a few microseconds keeps the cost of handing out pieces small. The first exception a job throws is thrown
    * here once every piece has finished (the rest still run). Throws std::invalid_argument for a grain of 0.
    */
    template <typename Work>
    void parallelFor(std::size_t count, std::size_t grain, Work&& work) {
        const RangeFunction function = [](void* context, std::size_t begin, std::size_t end) {
            Work& callable = *static_cast<std::remove_reference_t<Work>*>(context);
            for (std::size_t index = begin; index < end; ++index) {
                callable(index);
            }
        };
        runRange(count, grain, function, const_cast<void*>(static_cast<const void*>(&work)));
    }

private:
    // A piece of a parallelFor's range, through a plain function pointer so a call does not allocate.
    using RangeFunction = void (*)(void* context, std::size_t begin, std::size_t end);

    // One parallelFor call: its work, and how many of its indices have not finished yet.
    struct RangeTask {
        RangeFunction function = nullptr;
        void* context = nullptr;
        std::size_t grain = 1;
        std::atomic<std::size_t> remaining{0};
        std::mutex errorMutex;
        std::exception_ptr error;
    };

    struct Job {
        RangeTask* task = nullptr;
        std::size_t begin = 0;
        std::size_t end = 0;
    };

    struct Pile;

    void runRange(std::size_t count, std::size_t grain, RangeFunction function, void* context);

    // Split a job down to its grain, putting the upper halves on the thread's pile, then run what is left.
    void runJob(Job job, std::size_t pile);

    void push(std::size_t pile, const Job& job);

    // The thread's own newest job, or else another pile's oldest. False when every pile is empty.
    bool take(std::size_t pile, Job& job);

    void workerLoop(std::size_t pile);

    void stopWorkers();

    // Which pile the calling thread uses: its own for a worker of this system, pile 0 for any other thread.
    std::size_t currentPile() const;

    std::vector<std::unique_ptr<Pile>> piles;
    std::vector<std::thread> workers;

    // Jobs waiting in the piles, so sleeping workers know when to wake.
    std::atomic<std::size_t> queuedJobs{0};
    std::atomic<unsigned> sleepingWorkers{0};
    std::atomic<bool> stopping{false};
    std::mutex sleepMutex;
    std::condition_variable wakeUp;
};
