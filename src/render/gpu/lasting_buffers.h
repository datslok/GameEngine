#pragma once

#include "scene/frame_description.h"

#include <cstddef>
#include <span>
#include <stdexcept>
#include <vector>

/*
* Records kept in GPU memory from frame to frame (docs/research/GPUDRIVEN.md, stage 7): written only where they change,
* through a frame's own staging bytes, so standing still copies nothing and a change copies its own records. The
* renderer records the copies at the start of the frame's commands (render/vulkan/vulkan_lasting_buffer.h). API-free.
*/

// count records from data, for a lasting buffer from its record first on.
struct LastingWrite {
    std::size_t first = 0;
    const std::byte* data = nullptr;
    std::size_t count = 0;
};

// A copy from a frame's staging bytes into a lasting buffer, in bytes.
struct LastingCopy {
    std::size_t staging = 0;
    std::size_t target = 0;
    std::size_t bytes = 0;
};

/*
* The writes' records staged one after another in their order, each write its copy, joined with the copy before when
* its records follow that one's in both the staging bytes and the buffer. Empty writes are left out.
*/
std::vector<LastingCopy> planLastingCopies(std::span<const LastingWrite> writes, std::size_t recordBytes);

// The staging bytes the copies read.
std::size_t stagedBytes(std::span<const LastingCopy> copies);

// The records a lasting buffer has room for: what it has when enough, else twice that or what is needed, at least smallest.
std::size_t lastingCapacity(std::size_t needed, std::size_t current, std::size_t smallest);

// Writes for the records ranges name (joined first, joinDrawRanges), placed in the buffer offset records on.
template<typename Record>
void appendLastingWrites(std::vector<LastingWrite>& writes, const std::vector<DrawRange>& ranges, std::span<const Record> records, std::size_t offset) {
    for (const DrawRange& range : joinDrawRanges(ranges)) {
        if (range.first + range.count > records.size()) {
            throw std::out_of_range("A lasting write names records past those given");
        }

        writes.push_back(LastingWrite{offset + range.first, reinterpret_cast<const std::byte*>(records.data() + range.first), range.count});
    }
}
