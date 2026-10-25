#include "core/profiler.h"
#include "engine/debug_views.h"

#include <cassert>
#include <cmath>
#include <stdexcept>
#include <string>

namespace {
    bool nearlyEqual(double actual, double expected) {
        return std::abs(actual - expected) < 1e-12;
    }
}

void testProfiler() {
    // Sections are numbered in the order they are added and keep their names.
    {
        Profiler profiler{0.5};
        const int ticks = profiler.addSection("TICK");
        const int render = profiler.addSection("RENDER");
        assert(ticks == 0);
        assert(render == 1);
        assert(profiler.getSectionCount() == 2);
        assert(profiler.getName(render) == "RENDER");
    }

    // Nothing is shown until a whole window has passed: a section starts at zero, with no division by zero.
    {
        Profiler profiler{0.5};
        const int section = profiler.addSection("TICK");
        profiler.update(10.0);
        profiler.record(section, 0.002);
        profiler.update(10.3);

        const SectionTiming& timing = profiler.getTiming(section);
        assert(timing.calls == 0);
        assert(timing.averageSeconds == 0.0);
        assert(timing.worstSeconds == 0.0);
    }

    // When the window ends, each section shows the average and the worst of one call, and how many calls there were,
    // however the calls fell into frames (several in one frame, none in another).
    {
        Profiler profiler{0.5};
        const int section = profiler.addSection("TICK");
        profiler.update(0.0);

        profiler.record(section, 0.001);
        profiler.record(section, 0.003);
        profiler.update(0.2);
        profiler.update(0.4);
        profiler.record(section, 0.002);
        profiler.update(0.5);

        const SectionTiming& timing = profiler.getTiming(section);
        assert(timing.calls == 3);
        assert(nearlyEqual(timing.averageSeconds, 0.002));
        assert(nearlyEqual(timing.worstSeconds, 0.003));
    }

    // The shown numbers stay put during the next window, then are replaced by that window's alone.
    {
        Profiler profiler{0.5};
        const int section = profiler.addSection("TICK");
        profiler.update(0.0);
        profiler.record(section, 0.004);
        profiler.update(0.5);

        profiler.record(section, 0.001);
        profiler.update(0.7);
        assert(nearlyEqual(profiler.getTiming(section).averageSeconds, 0.004));

        profiler.update(1.0);
        const SectionTiming& timing = profiler.getTiming(section);
        assert(timing.calls == 1);
        assert(nearlyEqual(timing.averageSeconds, 0.001));
        assert(nearlyEqual(timing.worstSeconds, 0.001));
    }

    // A section that did not run in a window shows zero again rather than keeping old numbers.
    {
        Profiler profiler{0.5};
        const int section = profiler.addSection("GRENADES");
        profiler.update(0.0);
        profiler.record(section, 0.004);
        profiler.update(0.5);
        profiler.update(1.0);
        const SectionTiming& timing = profiler.getTiming(section);
        assert(timing.calls == 0);
        assert(timing.averageSeconds == 0.0);
        assert(timing.worstSeconds == 0.0);
    }

    // A ScopedTimer adds the time its block took when the block ends, so code that ran measures more than nothing.
    {
        Profiler profiler{0.5};
        const int section = profiler.addSection("WORK");
        const double start = profilerSeconds();
        profiler.update(start);
        {
            ScopedTimer timer{profiler, section};
            volatile double sum = 0.0;
            for (int i = 0; i < 100000; ++i) {
                sum = sum + std::sqrt(static_cast<double>(i));
            }
        }
        profiler.update(start + 1.0);
        const SectionTiming& timing = profiler.getTiming(section);
        assert(timing.calls == 1);
        assert(timing.averageSeconds > 0.0);
        assert(timing.averageSeconds == timing.worstSeconds);
    }

    // timeSection runs the work once and records it as one call.
    {
        Profiler profiler{0.5};
        const int section = profiler.addSection("SYSTEM");
        profiler.update(0.0);
        int runs = 0;
        timeSection(profiler, section, [&] { ++runs; });
        assert(runs == 1);
        profiler.update(0.5);
        assert(profiler.getTiming(section).calls == 1);
    }

    // The clock never goes backwards.
    {
        const double first = profilerSeconds();
        const double second = profilerSeconds();
        assert(second >= first);
    }

    // The readout: each section's name, then its average (and worst, if asked) per call in milliseconds.
    {
        Profiler profiler{0.5};
        const int frame = profiler.addSection("FRAME");
        const int work = profiler.addSection("WORK");
        profiler.update(0.0);
        profiler.record(frame, 0.004);
        profiler.record(frame, 0.006);
        profiler.record(work, 0.0012344);
        profiler.update(0.5);

        assert(profileText(profiler, 2, true) == "FRAME 5.00/6.00  WORK 1.23/1.23  MS");
        assert(profileText(profiler, 3, false) == "FRAME 5.000  WORK 1.234  MS");
    }

    // Hiding idle sections leaves out those that took no time, but always keeps the first (the total).
    {
        Profiler profiler{0.5};
        const int total = profiler.addSection("TOTAL");
        profiler.addSection("IDLE");
        const int busy = profiler.addSection("BUSY");
        profiler.update(0.0);
        profiler.record(total, 0.0);
        profiler.record(busy, 0.002);
        profiler.update(0.5);

        assert(profileText(profiler, 1, false, true) == "TOTAL 0.0  BUSY 2.0  MS");
        assert(profileText(profiler, 1, false) == "TOTAL 0.0  IDLE 0.0  BUSY 2.0  MS");
    }

    // A window of no length would publish every frame and one of negative length makes no sense.
    {
        bool threw = false;
        try {
            Profiler profiler{0.0};
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }
}
