#include "core/background_work.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

    /*
    * Work spread over loading frames: steps in order from where the last frame stopped, until the budget has passed
    * (the clock read after each step), at least one while any are left, none past the total. A clock moving 10 ms a
    * reading and a 30 ms budget gives three steps a frame.
    */
    void testStepsWithinBudget() {
        double now = 0.0;
        const auto clock = [&] {
            const double reading = now;
            now += 0.01;
            return reading;
        };
        std::vector<std::size_t> taken;
        const auto step = [&](std::size_t index) { taken.push_back(index); };

        std::size_t done = stepsWithinBudget(0, 8, 0.03, clock, step);
        assert(done == 3 && taken == (std::vector<std::size_t>{0, 1, 2}));
        done = stepsWithinBudget(done, 8, 0.03, clock, step);
        assert(done == 6);
        done = stepsWithinBudget(done, 8, 0.03, clock, step);
        assert(done == 8 && taken.size() == 8 && taken.back() == 7);
        assert(stepsWithinBudget(8, 8, 0.03, clock, step) == 8 && taken.size() == 8);
        assert(stepsWithinBudget(0, 8, 0.0, clock, step) == 1);
    }
}

void testBackgroundWork() {
    testQuickWork();
    testWaitingWhileWorking();
    testWorkThrows();
    testWaitingThrows();
    testStepsWithinBudget();
}
