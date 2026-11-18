#include "render/gpu/lasting_buffers.h"

#include <algorithm>

std::vector<LastingCopy> planLastingCopies(std::span<const LastingWrite> writes, std::size_t recordBytes) {
    std::vector<LastingCopy> copies;
    std::size_t staging = 0;

    for (const LastingWrite& write : writes) {
        if (write.count == 0) {
            continue;
        }

        const std::size_t bytes = write.count * recordBytes;
        const std::size_t target = write.first * recordBytes;

        if (!copies.empty() && copies.back().target + copies.back().bytes == target) {
            copies.back().bytes += bytes;
        } else {
            copies.push_back(LastingCopy{staging, target, bytes});
        }

        staging += bytes;
    }

    return copies;
}

std::size_t stagedBytes(std::span<const LastingCopy> copies) {
    return copies.empty() ? 0 : copies.back().staging + copies.back().bytes;
}

std::size_t lastingCapacity(std::size_t needed, std::size_t current, std::size_t smallest) {
    if (needed <= current && current >= smallest) {
        return current;
    }

    return std::max({needed, 2 * current, smallest});
}
