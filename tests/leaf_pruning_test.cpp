#include "scene/leaf_pruning.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <vector>

namespace {
    // Each level keeps half the parts of the one below, rounded up, down to one; and there are as many levels as that takes.
    void testEachLevelKeepsHalf() {
        assert(prunedKept(15, 0) == 15 && prunedKept(15, 1) == 8 && prunedKept(15, 2) == 4 && prunedKept(15, 3) == 2 && prunedKept(15, 4) == 1);
        assert(prunedKept(16, 4) == 1 && prunedKept(0, 0) == 0);
        assert(prunedLevels(15) == 5 && prunedLevels(16) == 5 && prunedLevels(2) == 2 && prunedLevels(1) == 1 && prunedLevels(0) == 1);
    }

    /*
    * The order holds every part once; each level's prefix is exactly the parts numbered (from the offset) a multiple of
    * 2^level, so a level's parts are a subset of the finer level's and spread evenly; at the first halving no two
    * neighbours are both gone.
    */
    void testTheOrderSpreadsTheSurvivors() {
        for (std::uint32_t count = 1; count <= 40; ++count) {
            for (std::uint32_t offset = 0; offset < count + 3; ++offset) {
                const std::vector<std::uint32_t> order = prunedOrder(count, offset);
                std::vector<std::uint32_t> sorted = order;
                std::sort(sorted.begin(), sorted.end());

                for (std::uint32_t part = 0; part < count; ++part) {
                    assert(sorted[part] == part);
                }

                for (int level = 0; level < prunedLevels(count); ++level) {
                    const std::uint32_t step = 1u << level;

                    for (std::uint32_t kept = 0; kept < prunedKept(count, level); ++kept) {
                        assert((order[kept] + offset) % count % step == 0);
                    }
                }

                std::vector<bool> keptAtOne(count, false);

                for (std::uint32_t kept = 0; kept < prunedKept(count, 1); ++kept) {
                    keptAtOne[order[kept]] = true;
                }

                for (std::uint32_t part = 0; part + 1 < count; ++part) {
                    assert(keptAtOne[part] || keptAtOne[part + 1]);
                }
            }
        }
    }

    // Twigs with the same number of leaves but different offsets lose different leaves.
    void testTheOffsetChangesWhichGo() {
        const std::vector<std::uint32_t> first = prunedOrder(15, 0);
        const std::vector<std::uint32_t> second = prunedOrder(15, 1);
        std::vector<std::uint32_t> keptFirst(first.begin(), first.begin() + prunedKept(15, 1));
        std::vector<std::uint32_t> keptSecond(second.begin(), second.begin() + prunedKept(15, 1));
        std::sort(keptFirst.begin(), keptFirst.end());
        std::sort(keptSecond.begin(), keptSecond.end());
        assert(keptFirst != keptSecond);
    }
}

void testLeafPruning() {
    testEachLevelKeepsHalf();
    testTheOrderSpreadsTheSurvivors();
    testTheOffsetChangesWhichGo();
}
