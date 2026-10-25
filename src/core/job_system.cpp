#include "core/job_system.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <stdexcept>

namespace {
    // The system and pile of the worker running on this thread; null on any thread that is not a worker.
    thread_local const JobSystem* workerSystem = nullptr;
    thread_local std::size_t workerPile = 0;
}

/*
* A thread's pile of jobs, behind a lock. Lock-free piles (the Chase-Lev deque) avoid the lock but are much harder to
* get right; with pieces of a few microseconds or more, the lock costs little. Each pile sits on its own cache lines,
* so threads taking from their own piles do not slow each other down.
*/
struct alignas(64) JobSystem::Pile {
    std::mutex mutex;
    std::deque<Job> jobs;
};

unsigned defaultWorkerCount() {
    return std::max(1u, std::thread::hardware_concurrency()) - 1;
}

JobSystem& serialJobs() {
    static JobSystem serial{0};
    return serial;
}

JobSystem::JobSystem(unsigned workerCount) {
    for (unsigned pile = 0; pile <= workerCount; ++pile) {
        piles.push_back(std::make_unique<Pile>());
    }

    try {
        for (unsigned worker = 0; worker < workerCount; ++worker) {
            workers.emplace_back([this, worker] { workerLoop(worker + 1); });
        }
    } catch (...) {
        stopWorkers();
        throw;
    }
}

JobSystem::~JobSystem() {
    stopWorkers();
}

// Wake every worker and wait for it to finish (it leaves once no jobs are left), so none outlives the piles.
void JobSystem::stopWorkers() {
    {
        const std::lock_guard<std::mutex> lock{sleepMutex};
        stopping.store(true);
    }
    wakeUp.notify_all();

    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers.clear();
}

unsigned JobSystem::getThreadCount() const {
    return static_cast<unsigned>(workers.size()) + 1;
}

std::size_t JobSystem::currentPile() const {
    return workerSystem == this ? workerPile : 0;
}

/*
* With no workers, or a range of one piece, the work runs here and now, with nothing to hand out. Otherwise the calling
* thread starts the range itself (splitting it onto its pile, where idle workers steal the halves) and then helps with
* any job until every index of this range has finished. The task lives on this stack frame, which is safe because
* nothing touches it after its last index is counted off.
*/
void JobSystem::runRange(std::size_t count, std::size_t grain, RangeFunction function, void* context) {
    if (grain == 0) {
        throw std::invalid_argument("parallelFor needs a grain of at least one index");
    }
    if (count == 0) {
        return;
    }
    if (workers.empty() || count <= grain) {
        function(context, 0, count);
        return;
    }

    RangeTask task;
    task.function = function;
    task.context = context;
    task.grain = grain;
    task.remaining.store(count);

    const std::size_t pile = currentPile();
    runJob(Job{&task, 0, count}, pile);

    Job job;
    while (task.remaining.load(std::memory_order_acquire) != 0) {
        if (take(pile, job)) {
            runJob(job, pile);
        } else {
            std::this_thread::yield();
        }
    }

    if (task.error) {
        std::rethrow_exception(task.error);
    }
}

/*
* Halving rather than cutting the range into grain-sized pieces up front: a thief takes the oldest job, which is the
* biggest half left, so a few steals spread the work, and each thread then splits its own half further.
*/
void JobSystem::runJob(Job job, std::size_t pile) {
    RangeTask& task = *job.task;
    while (job.end - job.begin > task.grain) {
        const std::size_t middle = job.begin + (job.end - job.begin) / 2;
        push(pile, Job{&task, middle, job.end});
        job.end = middle;
    }

    try {
        task.function(task.context, job.begin, job.end);
    } catch (...) {
        const std::lock_guard<std::mutex> lock{task.errorMutex};
        if (!task.error) {
            task.error = std::current_exception();
        }
    }

    // The last thing done with the task: once its count reaches zero, the waiting thread may return and destroy it.
    task.remaining.fetch_sub(job.end - job.begin, std::memory_order_acq_rel);
}

/*
* A sleeping worker counts itself before checking for jobs, and a push counts its job before checking for sleepers
* (both sequentially consistent), so at least one sees the other: either the worker finds the job, or the push wakes
* it. Pushes skip the wake-up lock while every worker is busy.
*/
void JobSystem::push(std::size_t pile, const Job& job) {
    {
        const std::lock_guard<std::mutex> lock{piles[pile]->mutex};
        piles[pile]->jobs.push_back(job);
    }
    queuedJobs.fetch_add(1);

    if (sleepingWorkers.load() > 0) {
        {
            const std::lock_guard<std::mutex> lock{sleepMutex};
        }
        wakeUp.notify_one();
    }
}

bool JobSystem::take(std::size_t pile, Job& job) {
    {
        Pile& own = *piles[pile];
        const std::lock_guard<std::mutex> lock{own.mutex};
        if (!own.jobs.empty()) {
            job = own.jobs.back();
            own.jobs.pop_back();
            queuedJobs.fetch_sub(1);
            return true;
        }
    }

    for (std::size_t step = 1; step < piles.size(); ++step) {
        Pile& other = *piles[(pile + step) % piles.size()];
        const std::lock_guard<std::mutex> lock{other.mutex};
        if (!other.jobs.empty()) {
            job = other.jobs.front();
            other.jobs.pop_front();
            queuedJobs.fetch_sub(1);
            return true;
        }
    }
    return false;
}

void JobSystem::workerLoop(std::size_t pile) {
    workerSystem = this;
    workerPile = pile;

    Job job;
    while (true) {
        if (take(pile, job)) {
            runJob(job, pile);
            continue;
        }

        // Look again for a little while (giving the core to others between looks) before sleeping.
        const auto spinUntil = std::chrono::steady_clock::now() + std::chrono::duration<double>(workerSpinSeconds);
        bool found = false;
        while (!found && std::chrono::steady_clock::now() < spinUntil && !stopping.load()) {
            std::this_thread::yield();
            found = queuedJobs.load() > 0 && take(pile, job);
        }
        if (found) {
            runJob(job, pile);
            continue;
        }

        std::unique_lock<std::mutex> lock{sleepMutex};
        sleepingWorkers.fetch_add(1);
        wakeUp.wait(lock, [this] { return queuedJobs.load() > 0 || stopping.load(); });
        sleepingWorkers.fetch_sub(1);

        if (stopping.load() && queuedJobs.load() == 0) {
            return;
        }
    }
}
