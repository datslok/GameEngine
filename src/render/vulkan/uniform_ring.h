#pragma once

#include <cstddef>
#include <optional>

// The smallest multiple of alignment (a power of two) that is at least value.
constexpr std::size_t alignUp(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

/*
* Hands out slices of one buffer for a frame's uniform data, front to back, each starting on the GPU's alignment for
* uniform buffer offsets. Shaders read a slice through a descriptor of a fixed range (bindRange) placed at its offset,
* so every slice leaves room for a whole range inside the buffer. The arithmetic only: the renderer owns the buffer
* and calls reset() once the GPU has finished the frame that last used it.
*/
class UniformRing {
public:
    // Throws std::invalid_argument unless alignment is a power of two and the buffer holds at least one whole range.
    UniformRing(std::size_t capacity, std::size_t alignment, std::size_t bindRange);

    // The next slice's offset, or none when the ring is full. Throws std::invalid_argument for a slice larger than
    // bindRange, which no descriptor could read whole.
    std::optional<std::size_t> tryAllocate(std::size_t bytes);

    // As tryAllocate, but throws std::length_error when full.
    std::size_t allocate(std::size_t bytes);

    void reset();

    std::size_t getUsedBytes() const;
    std::size_t getCapacity() const;

private:
    std::size_t capacity;
    std::size_t alignment;
    std::size_t bindRange;
    std::size_t used = 0;
};
