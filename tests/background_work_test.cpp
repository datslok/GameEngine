#include "core/background_work.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
    using namespace std::chrono_literals;

    // Quick work: done when it returns, and the waiting side was called at least once (the first loading frame at once).
    void testQuickWork() {
        bool worked = false;
        int waits = 0;
        runWhileWaiting([&] { worked = true; }, [&] { ++waits; }, 1ms);
        assert(worked && waits >= 1);
    }

    // Long work: the waiting side keeps being called while it runs (here the work cannot end until it has been three times).
    void testWaitingWhileWorking() {
        std::atomic<int> waits = 0;
        runWhileWaiting(
            [&] {
                while (waits.load() < 3) {
                }
            },
            [&] { ++waits; }, 1ms);
        assert(waits.load() >= 3);
    }

    // What the work throws reaches the caller, as it would have without the thread.
    void testWorkThrows() {
        bool caught = false;

        try {
            runWhileWaiting([] { throw std::runtime_error("no valley"); }, [] {}, 1ms);
        } catch (const std::runtime_error& error) {
            caught = std::string{error.what()} == "no valley";
        }

        assert(caught);
    }

    // What the waiting side throws reaches the caller too, but only once the work has finished (its thread is never left running).
    void testWaitingThrows() {
        std::atomic<bool> finished = false;
        bool caught = false;

        try {
            runWhileWaiting(
                [&] {
                    std::this_thread::sleep_for(5ms);
                    finished = true;
                },
                [] { throw std::runtime_error("window lost"); }, 1ms);
        } catch (const std::runtime_error&) {
            caught = true;
        }

        assert(caught && finished.load());
    }
}

void testBackgroundWork() {
    testQuickWork();
    testWaitingWhileWorking();
    testWorkThrows();
    testWaitingThrows();
}
