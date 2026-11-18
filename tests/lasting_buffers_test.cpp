#include "render/gpu/lasting_buffers.h"
#include "scene/frame_description.h"

#include <array>
#include <cassert>
#include <cstddef>
#include <vector>

namespace {
    bool sameCopy(const LastingCopy& copy, std::size_t staging, std::size_t target, std::size_t bytes) {
        return copy.staging == staging && copy.target == target && copy.bytes == bytes;
    }

    // Ranges sorted and joined where they touch or overlap; empty ones dropped.
    void testRangesJoin() {
        const std::vector<DrawRange> joined = joinDrawRanges({DrawRange{10, 2}, DrawRange{0, 3}, DrawRange{3, 1}, DrawRange{5, 0}, DrawRange{11, 4}});
        assert(joined.size() == 2);
        assert(joined[0].first == 0 && joined[0].count == 4);
        assert(joined[1].first == 10 && joined[1].count == 5);
        assert(joinDrawRanges({}).empty());
    }

    // Writes staged one after another in their order; writes whose targets follow each other are one copy.
    void testCopiesArePlanned() {
        const std::array<std::byte, 160> bytes{};
        const std::vector<LastingWrite> writes{
            LastingWrite{0, bytes.data(), 3}, LastingWrite{3, bytes.data(), 2}, LastingWrite{7, bytes.data(), 0}, LastingWrite{10, bytes.data(), 1},
            LastingWrite{2, bytes.data(), 1},
        };
        const std::vector<LastingCopy> copies = planLastingCopies(writes, 16);
        assert(copies.size() == 3);
        assert(sameCopy(copies[0], 0, 0, 80));
        assert(sameCopy(copies[1], 80, 160, 16));
        assert(sameCopy(copies[2], 96, 32, 16));
        assert(stagedBytes(copies) == 112);
        assert(planLastingCopies({}, 16).empty() && stagedBytes({}) == 0);
    }

    // Writes made from ranges over records: joined first, each pointing at its records, placed after an offset.
    void testWritesFromRanges() {
        const std::array<int, 8> records{0, 1, 2, 3, 4, 5, 6, 7};
        std::vector<LastingWrite> writes;
        appendLastingWrites(writes, {DrawRange{4, 2}, DrawRange{0, 2}, DrawRange{2, 1}}, std::span<const int>{records}, 0);
        assert(writes.size() == 2);
        assert(writes[0].first == 0 && writes[0].count == 3 && writes[0].data == reinterpret_cast<const std::byte*>(&records[0]));
        assert(writes[1].first == 4 && writes[1].count == 2 && writes[1].data == reinterpret_cast<const std::byte*>(&records[4]));

        // A tail after 8 kept records: all of it, at 8 on.
        appendLastingWrites(writes, {DrawRange{0, 3}}, std::span<const int>{records}.first(3), 8);
        assert(writes.size() == 3 && writes[2].first == 8 && writes[2].count == 3);

        // Ranges past the records are a mistake.
        bool threw = false;

        try {
            appendLastingWrites(writes, {DrawRange{6, 4}}, std::span<const int>{records}, 0);
        } catch (const std::out_of_range&) {
            threw = true;
        }

        assert(threw);
    }

    // Room kept when enough, else twice it or what is needed, never below the smallest.
    void testCapacityGrows() {
        assert(lastingCapacity(10, 0, 64) == 64);
        assert(lastingCapacity(50, 64, 64) == 64);
        assert(lastingCapacity(100, 64, 64) == 128);
        assert(lastingCapacity(300, 64, 64) == 300);
    }
}

void testLastingBuffers() {
    testRangesJoin();
    testCopiesArePlanned();
    testWritesFromRanges();
    testCapacityGrows();
}
