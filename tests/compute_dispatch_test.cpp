#include "render/gpu/compute_dispatch.h"

#include <cassert>
#include <stdexcept>

namespace {
    // Enough groups to give every item an invocation, rounding up: the last group's spare invocations do nothing.
    void testGroupsCoverEveryItem() {
        assert(workgroupCount(0, 64) == 0);
        assert(workgroupCount(1, 64) == 1);
        assert(workgroupCount(64, 64) == 1);
        assert(workgroupCount(65, 64) == 2);
        assert(workgroupCount(1000, 64) == 16);
        assert(workgroupCount(7, 1) == 7);
    }

    // No overflow for counts near the top of the range, where count + size - 1 would wrap around.
    void testLargeCounts() {
        assert(workgroupCount(0xFFFFFFFFu, 64) == 0x4000000u);
        assert(workgroupCount(0xFFFFFFFFu, 1) == 0xFFFFFFFFu);
    }

    void testEmptyGroupsAreRejected() {
        bool threw = false;

        try {
            workgroupCount(10, 0);
        } catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
    }
}

void testComputeDispatch() {
    testGroupsCoverEveryItem();
    testLargeCounts();
    testEmptyGroupsAreRejected();
}
