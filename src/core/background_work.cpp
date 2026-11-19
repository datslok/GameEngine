#include "core/background_work.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

/*
* The work's thread raises done under the lock and wakes this one, which otherwise wakes every interval to call waiting
* again. The work's exception is kept and rethrown after the join; a throw from waiting first waits for the work.
*/
void runWhileWaiting(const std::function<void()>& work, const std::function<void()>& waiting, std::chrono::milliseconds interval) {
    std::mutex lock;
    std::condition_variable finished;
    bool done = false;
    std::exception_ptr workError;

    std::thread worker([&] {
        try {
            work();
        } catch (...) {
            workError = std::current_exception();
        }

        const std::lock_guard<std::mutex> guard(lock);
        done = true;
        finished.notify_one();
    });

    const auto waitForWork = [&] {
        std::unique_lock<std::mutex> guard(lock);
        finished.wait(guard, [&] { return done; });
    };

    try {
        while (true) {
            waiting();
            std::unique_lock<std::mutex> guard(lock);

            if (finished.wait_for(guard, interval, [&] { return done; })) {
                break;
            }
        }
    } catch (...) {
        waitForWork();
        worker.join();
        throw;
    }

    worker.join();

    if (workError) {
        std::rethrow_exception(workError);
    }
}

std::size_t stepsWithinBudget(std::size_t done, std::size_t total, double budgetSeconds, const std::function<double()>& clock,
                              const std::function<void(std::size_t)>& step) {
    if (done >= total) {
        return done;
    }

    const double start = clock();

    while (done < total) {
        step(done);
        ++done;

        if (clock() - start >= budgetSeconds) {
            break;
        }
    }

    return done;
}
