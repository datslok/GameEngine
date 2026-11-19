#include "core/startup_clock.h"

#include <cassert>
#include <string>

namespace {
    // Each lap is the time since the one before (the first since the start); the total runs from the start to the last.
    void testLaps() {
        StartupClock clock{10.0};
        clock.lap("window", 10.25);
        clock.lap("renderer", 12.5);
        clock.lap("world", 20.0);
        assert(clock.laps().size() == 3);
        assert(clock.laps()[0].name == "window" && clock.laps()[0].seconds == 0.25);
        assert(clock.laps()[1].seconds == 2.25 && clock.laps()[2].seconds == 7.5);
        assert(clock.total() == 10.0);

        StartupClock none{3.0};
        assert(none.laps().empty() && none.total() == 0.0);
    }

    // One line for the log: the title and the total, then each lap, in seconds to two places.
    void testDescription() {
        StartupClock clock{0.0};
        clock.lap("window", 0.123);
        clock.lap("renderer", 2.5);
        assert(clock.describe("Start-up") == "Start-up 2.50 s: window 0.12 s, renderer 2.38 s");
        assert(StartupClock{1.0}.describe("Nothing") == "Nothing 0.00 s");
    }
}

void testStartupClock() {
    testLaps();
    testDescription();
}
