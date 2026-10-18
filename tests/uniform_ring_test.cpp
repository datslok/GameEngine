#include "render/vulkan/uniform_ring.h"

#include <cassert>
#include <stdexcept>

namespace {
    // Each slice starts on the GPU's alignment (256 bytes on most desktop GPUs), however small the one before it.
    void testSlicesAreAligned() {
        UniformRing ring{4096, 256, 512};

        assert(ring.allocate(64) == 0);
        assert(ring.allocate(64) == 256);
        assert(ring.allocate(300) == 512);
        assert(ring.allocate(16) == 1024);
        assert(ring.getUsedBytes() == 1040);
    }

    /*
    * A slice is read through a descriptor of a fixed range (bindRange) placed at the slice's offset, so the whole
    * range must fit in the buffer even when the slice itself is smaller. Here 4096 bytes with a 1024-byte range: the
    * last slice may start at 3072.
    */
    void testWholeBindRangeFits() {
        UniformRing ring{4096, 1024, 1024};

        assert(ring.tryAllocate(16) == 0);
        assert(ring.tryAllocate(16) == 1024);
        assert(ring.tryAllocate(16) == 2048);
        assert(ring.tryAllocate(16) == 3072);
        assert(!ring.tryAllocate(16).has_value());
    }

    // allocate() is for a ring that cannot run out; tryAllocate() lets the caller move on to another buffer.
    void testFullRingThrows() {
        UniformRing ring{1024, 256, 256};

        for (int slice = 0; slice < 4; ++slice) {
            ring.allocate(256);
        }

        bool threw = false;

        try {
            ring.allocate(1);
        }
        catch (const std::length_error&) {
            threw = true;
        }

        assert(threw);
    }

    // A slice larger than the range a descriptor reads could never be read whole.
    void testSliceLargerThanRangeIsRejected() {
        UniformRing ring{4096, 256, 512};
        bool threw = false;

        try {
            ring.allocate(513);
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
    }

    // Each frame starts over once the GPU has finished with the last frame that used the ring.
    void testResetStartsOver() {
        UniformRing ring{4096, 256, 512};
        ring.allocate(100);
        ring.allocate(100);
        ring.reset();

        assert(ring.getUsedBytes() == 0);
        assert(ring.allocate(100) == 0);
    }

    void testAlignUp() {
        assert(alignUp(0, 256) == 0);
        assert(alignUp(1, 256) == 256);
        assert(alignUp(256, 256) == 256);
        assert(alignUp(257, 64) == 320);
    }

    // The GPU's alignment is a power of two, and a ring must hold at least one whole range.
    void testBadShapesAreRejected() {
        bool threwForAlignment = false;
        bool threwForSize = false;

        try {
            UniformRing ring{4096, 100, 256};
        }
        catch (const std::invalid_argument&) {
            threwForAlignment = true;
        }

        try {
            UniformRing ring{256, 256, 512};
        }
        catch (const std::invalid_argument&) {
            threwForSize = true;
        }

        assert(threwForAlignment);
        assert(threwForSize);
    }
}

void testUniformRing() {
    testSlicesAreAligned();
    testWholeBindRangeFits();
    testFullRingThrows();
    testSliceLargerThanRangeIsRejected();
    testResetStartsOver();
    testAlignUp();
    testBadShapesAreRejected();
}
