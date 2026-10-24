#include "render/gpu/deforming_slots.h"

#include <cassert>
#include <cstdint>
#include <deque>
#include <set>

namespace {
    // A frame's reads: the slots of the current and the previous shape.
    struct FrameReads {
        std::uint32_t current = 0;
        std::uint32_t previous = 0;
    };

    /*
    * With F frames in flight, frame N starts once frame N - F has finished, so frames N - F + 1 .. N - 1 may still be
    * reading. Whatever the order of new shapes and frames without one, the slot frame N writes is none of theirs.
    */
    void checkNeverWritesASlotInFlight(std::uint32_t framesInFlight, std::uint32_t pattern) {
        DeformingSlots slots;
        std::deque<FrameReads> inFlight;

        for (int frame = 0; frame < 64; ++frame) {
            const bool changes = ((pattern >> (frame % 32)) & 1u) != 0;

            if (inFlight.size() >= framesInFlight) {
                inFlight.pop_front();
            }

            if (changes) {
                const std::uint32_t written = slots.advance();
                assert(written < deformingSlotCount);
                for (const FrameReads& reads : inFlight) {
                    assert(written != reads.current && written != reads.previous);
                }
                assert(slots.current() == written);
                assert(slots.changed());
            } else {
                slots.hold();
                assert(!slots.changed());
                assert(slots.previous() == slots.current());
            }

            inFlight.push_back(FrameReads{slots.current(), slots.previous()});
        }
    }
}

void testDeformingSlots() {
    // At the start the rest shape is in slot 0, and it has not moved.
    {
        const DeformingSlots slots;
        assert(slots.current() == 0);
        assert(slots.previous() == 0);
        assert(!slots.changed());
        assert(slots.version(0) == 1);
    }

    // A new shape goes into the next slot, the last one becomes the previous, and each written slot's version counts up,
    // so whatever was built from a slot can tell when it is out of date.
    {
        DeformingSlots slots;
        assert(slots.advance() == 1);
        assert(slots.previous() == 0);
        assert(slots.version(1) == 2);
        assert(slots.advance() == 2);
        assert(slots.previous() == 1);
        assert(slots.version(2) == 3);
        assert(slots.version(1) == 2);
    }

    // A frame without a new shape shows the last one still: no motion.
    {
        DeformingSlots slots;
        slots.advance();
        slots.hold();
        assert(slots.current() == 1);
        assert(slots.previous() == 1);
    }

    // There is one slot more than the frames that can be in flight, enough for any order.
    assert(deformingSlotCount == maxFramesInFlight + 1);
    for (std::uint32_t frames = 1; frames <= maxFramesInFlight; ++frames) {
        for (std::uint32_t pattern : {0xffffffffu, 0x0u, 0xaaaaaaaau, 0x55555555u, 0x9249249u, 0xf0f0f0f0u, 0x12345678u, 0xdeadbeefu}) {
            checkNeverWritesASlotInFlight(frames, pattern);
        }
    }
}
