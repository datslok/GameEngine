#include "scene/leaf_pruning.h"

#include <algorithm>

namespace {
    // value's lowest bits binary digits in reverse order.
    std::uint32_t reversedBits(std::uint32_t value, int bits) {
        std::uint32_t reversed = 0;

        for (int bit = 0; bit < bits; ++bit) {
            reversed = (reversed << 1) | ((value >> bit) & 1u);
        }

        return reversed;
    }
}

/*
* A part numbered j (from the offset) keeps through level k exactly when j's lowest k bits are zero, which is when its
* reversed number is below 2^(bits - k): so sorting by reversed number puts each level's parts first, and there are
* ceil(count / 2^k) of them.
*/
std::vector<std::uint32_t> prunedOrder(std::uint32_t count, std::uint32_t offset) {
    int bits = 0;

    while ((std::uint64_t{1} << bits) < count) {
        ++bits;
    }

    std::vector<std::uint32_t> order(count);
    std::vector<std::uint32_t> key(count);

    for (std::uint32_t part = 0; part < count; ++part) {
        order[part] = part;
        key[part] = reversedBits(static_cast<std::uint32_t>((std::uint64_t{part} + offset) % count), bits);
    }

    std::sort(order.begin(), order.end(), [&key](std::uint32_t a, std::uint32_t b) {
        return key[a] < key[b];
    });

    return order;
}

std::uint32_t prunedKept(std::uint32_t count, int level) {
    if (count == 0) {
        return 0;
    }

    if (level >= 32) {
        return 1;
    }

    const std::uint64_t step = std::uint64_t{1} << std::max(level, 0);
    return static_cast<std::uint32_t>((count + step - 1) / step);
}

int prunedLevels(std::uint32_t count) {
    int levels = 1;

    while (prunedKept(count, levels - 1) > 1) {
        ++levels;
    }

    return levels;
}
