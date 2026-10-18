#include "render/vulkan/uniform_ring.h"

#include <stdexcept>

UniformRing::UniformRing(std::size_t capacity, std::size_t alignment, std::size_t bindRange):
    capacity(capacity),
    alignment(alignment),
    bindRange(bindRange)
{
    if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
        throw std::invalid_argument("Uniform ring alignment must be a power of two");
    }

    if (bindRange == 0 || capacity < bindRange) {
        throw std::invalid_argument("Uniform ring must hold at least one whole bind range");
    }
}

std::optional<std::size_t> UniformRing::tryAllocate(std::size_t bytes) {
    if (bytes > bindRange) {
        throw std::invalid_argument("Uniform data is larger than the range a descriptor reads");
    }

    const std::size_t offset = alignUp(used, alignment);

    if (offset + bindRange > capacity) {
        return std::nullopt;
    }

    used = offset + bytes;
    return offset;
}

std::size_t UniformRing::allocate(std::size_t bytes) {
    const std::optional<std::size_t> offset = tryAllocate(bytes);

    if (!offset) {
        throw std::length_error("Uniform ring is full");
    }

    return *offset;
}

void UniformRing::reset() {
    used = 0;
}

std::size_t UniformRing::getUsedBytes() const {
    return used;
}

std::size_t UniformRing::getCapacity() const {
    return capacity;
}
