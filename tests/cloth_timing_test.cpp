#include "core/profiler.h"
#include "engine/debug_views.h"
#include "render/gpu/cloth_timing.h"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    bool nearlyEqual(double actual, double expected) {
        return std::abs(actual - expected) < 1e-15;
    }

    // Every kind of cloth pass has its own short upper-case name, for the readout's stroke font.
    void testNames() {
        std::set<std::string> names;
        for (std::size_t pass = 0; pass < clothPassCount; ++pass) {
            const std::string name = clothPassName(static_cast<ClothPass>(pass));
            assert(!name.empty());
            for (const char letter : name) {
                assert(letter >= 'A' && letter <= 'Z');
            }
            names.insert(name);
        }
        assert(names.size() == clothPassCount);
    }

    /*
    * One stamp before the first pass and one after each: each gap is credited to the pass it follows, and a kind that
    * ran several times (a substep's predict every substep) adds up. The total is the whole cloth work.
    */
    void testStampsIntoPasses() {
        const std::array<std::uint64_t, 5> ticks{100, 150, 175, 375, 400};
        const std::array<ClothPass, 4> kinds{ClothPass::Predict, ClothPass::Sweep, ClothPass::Predict, ClothPass::Finish};
        const ClothPassTimings timings = clothPassTimingsFromTicks(ticks, kinds, 2.0, 64);
        assert(timings.valid);
        assert(nearlyEqual(timings.seconds(ClothPass::Predict), 500e-9));
        assert(nearlyEqual(timings.seconds(ClothPass::Sweep), 50e-9));
        assert(nearlyEqual(timings.seconds(ClothPass::Finish), 50e-9));
        assert(timings.seconds(ClothPass::Gradient) == 0.0);
        assert(nearlyEqual(timings.totalSeconds, 600e-9));
    }

    // The counter rolls over past its valid bits, as the GPU's own timings allow for.
    void testRollOver() {
        const std::uint64_t top = std::uint64_t{1} << 36;
        const std::array<std::uint64_t, 2> ticks{top - 10, 5};
        const std::array<ClothPass, 1> kinds{ClothPass::Move};
        assert(nearlyEqual(clothPassTimingsFromTicks(ticks, kinds, 1.0, 36).seconds(ClothPass::Move), 15e-9));
    }

    /*
    * A frame with no stamps (no cloth, or not measured) or a queue without timestamps is not valid, so it does not count
    * as one that took no time; one with only the first stamp ran no passes. A stamp missing for a pass is an error.
    */
    void testEdges() {
        assert(!clothPassTimingsFromTicks({}, {}, 1.0, 64).valid);
        const std::array<std::uint64_t, 1> first{42};
        const ClothPassTimings none = clothPassTimingsFromTicks(first, {}, 1.0, 64);
        assert(none.valid && none.totalSeconds == 0.0);

        const std::array<std::uint64_t, 2> ticks{0, 10};
        const std::array<ClothPass, 1> kinds{ClothPass::Copy};
        assert(!clothPassTimingsFromTicks(ticks, kinds, 1.0, 0).valid);

        bool threw = false;
        try {
            clothPassTimingsFromTicks(ticks, {}, 1.0, 64);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    // The readout's cloth profiler: the total first, then one section per kind in ClothPass order, named like them.
    void testProfiler() {
        Profiler profiler = makeClothProfiler();
        assert(profiler.getSectionCount() == static_cast<int>(clothPassCount) + 1);
        assert(profiler.getName(0) == "CLOTH");
        for (std::size_t pass = 0; pass < clothPassCount; ++pass) {
            assert(profiler.getName(static_cast<int>(pass) + 1) == clothPassName(static_cast<ClothPass>(pass)));
        }

        profiler.update(0.0);
        ClothPassTimings timings;
        timings.valid = true;
        timings.passSeconds[static_cast<std::size_t>(ClothPass::Gradient)] = 0.002;
        timings.totalSeconds = 0.002;
        recordClothTimings(profiler, timings);
        recordClothTimings(profiler, ClothPassTimings{});
        profiler.update(defaultProfileWindowSeconds);
        assert(profiler.getTiming(0).calls == 1);
        assert(nearlyEqual(profiler.getTiming(static_cast<int>(ClothPass::Gradient) + 1).averageSeconds, 0.002));

        // Kinds that took no time (XPBD's passes while vertex block descent runs) are left off the line.
        const std::string line = profileText(profiler, 2, false, true);
        assert(line.find("CLOTH 2.00") != std::string::npos && line.find(std::string(clothPassName(ClothPass::Gradient)) + " 2.00") != std::string::npos);
        assert(line.find(clothPassName(ClothPass::Stretch)) == std::string::npos);
    }

    /*
    * A frame with more cloth passes than its stamps is not measured, and says so: it records nothing, and the readout's
    * cloth line names why instead of going blank, until a frame is measured again.
    */
    void testTooManyPasses() {
        ClothPassTimings overflowed;
        overflowed.overflowed = true;
        assert(!overflowed.valid);

        Profiler profiler = makeClothProfiler();
        profiler.update(0.0);
        recordClothTimings(profiler, overflowed);
        profiler.update(defaultProfileWindowSeconds);
        assert(profiler.getTiming(0).calls == 0);

        assert(clothReadoutText(profiler, true) == clothPassesNotMeasuredText);
        assert(clothReadoutText(profiler, false) == profileText(profiler, 2, false, true));
        assert(std::string(clothPassesNotMeasuredText).find("NOT MEASURED") != std::string::npos);
    }

    // Self-collision's pair counts as a readout line: the pairs found, those dropped past the room, the large items.
    void testSelfPairsText() {
        ClothSelfPairCounts counts;
        assert(!counts.measured);
        counts.measured = true;
        counts.found = 26000;
        counts.lost = 12;
        counts.large = 3;
        assert(clothSelfPairsText(counts) == "SELF PAIRS 26000  LOST 12  LARGE 3");
    }
}

void testClothTiming() {
    testNames();
    testStampsIntoPasses();
    testRollOver();
    testEdges();
    testProfiler();
    testTooManyPasses();
    testSelfPairsText();
}
