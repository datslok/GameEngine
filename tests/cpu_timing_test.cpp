#include "render/cpu_timing.h"

#include <cassert>
#include <set>
#include <string>

namespace {
    // Each lap gives the time since the last to its stage, adding up when a stage comes round again; the total is all of
    // it; a clock not started measures nothing.
    void testLapsAddUpByStage() {
        CpuStageClock clock;
        clock.start(10.0);
        clock.lap(CpuStage::Wait, 10.5);
        clock.lap(CpuStage::Camera, 10.75);
        clock.lap(CpuStage::Shadows, 11.0);
        clock.lap(CpuStage::Camera, 11.25);
        const CpuStageTimings timings = clock.finish();
        assert(timings.valid);
        assert(timings.seconds[static_cast<std::size_t>(CpuStage::Wait)] == 0.5);
        assert(timings.seconds[static_cast<std::size_t>(CpuStage::Camera)] == 0.5);
        assert(timings.seconds[static_cast<std::size_t>(CpuStage::Shadows)] == 0.25);
        assert(timings.seconds[static_cast<std::size_t>(CpuStage::Pack)] == 0.0);
        assert(timings.totalSeconds == 1.25);

        // Finished, it starts afresh: laps before the next start count nothing.
        clock.lap(CpuStage::Wait, 12.0);
        assert(!clock.finish().valid);
    }

    // Every stage has its own short upper-case name for the readout.
    void testStageNames() {
        std::set<std::string> names;

        for (std::size_t stage = 0; stage < cpuStageCount; ++stage) {
            const std::string name = cpuStageName(static_cast<CpuStage>(stage));
            assert(!name.empty() && name.size() <= 10 && name.find(' ') == std::string::npos);

            for (char letter : name) {
                assert(letter >= 'A' && letter <= 'Z');
            }

            names.insert(name);
        }

        assert(names.size() == cpuStageCount);
    }
}

void testCpuTiming() {
    testLapsAddUpByStage();
    testStageNames();
}
