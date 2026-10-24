#include "render/gpu/deforming_slots.h"

std::uint32_t DeformingSlots::current() const {
    return currentSlot;
}

std::uint32_t DeformingSlots::previous() const {
    return previousSlot;
}

std::uint32_t DeformingSlots::advance() {
    previousSlot = currentSlot;
    currentSlot = (currentSlot + 1) % deformingSlotCount;
    versions[currentSlot] = ++written;
    return currentSlot;
}

void DeformingSlots::hold() {
    previousSlot = currentSlot;
}

bool DeformingSlots::changed() const {
    return previousSlot != currentSlot;
}

std::uint64_t DeformingSlots::version(std::uint32_t slot) const {
    return versions.at(slot);
}
