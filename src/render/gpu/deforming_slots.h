#pragma once

#include "render/frames_in_flight.h"

#include <array>
#include <cstdint>

/*
* A deforming mesh's vertices take turns through a small ring of buffers ("slots"), one shape each. A frame reads two:
* the current shape, and the previous one for motion vectors. With F frames in flight, frame N may write while frames
* N - F + 1 .. N - 1 still read, and each of those read the current slot of its time and the one before it. Writing
* always goes into the slot after the current one, and the current slot moves at most one step a frame, so with F + 1
* slots the slot written is never one a frame in flight still reads (tested over every order of new shapes and frames
* without one). No fences or copies are needed beyond the frame's own.
*/
inline constexpr std::uint32_t deformingSlotCount = maxFramesInFlight + 1;

class DeformingSlots {
public:
    // The slot holding the latest shape, and the slot holding the shape a frame before it.
    std::uint32_t current() const;
    std::uint32_t previous() const;

    // A new shape this frame: returns the slot to write it to, which becomes current; the old current becomes previous.
    std::uint32_t advance();

    // No new shape this frame: the last one stands still (previous is current, so it has no motion).
    void hold();

    // Whether this frame brought a new shape (advance rather than hold).
    bool changed() const;

    // Counts up with every shape written, starting at 1 for the rest shape in slot 0, so whatever was built from a slot (an
    // acceleration structure) can tell when the slot has been written since.
    std::uint64_t version(std::uint32_t slot) const;

private:
    std::uint32_t currentSlot = 0;
    std::uint32_t previousSlot = 0;
    std::uint64_t written = 1;
    std::array<std::uint64_t, deformingSlotCount> versions{1};
};
