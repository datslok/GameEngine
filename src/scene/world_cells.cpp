#include "scene/world_cells.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void visit(CellSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Size", settings.size, {.min = 1.0f, .max = 100000.0f, .sliderMin = 16.0f, .sliderMax = 1024.0f, .unit = "m"});
    visitor.field("Spare share", settings.spareShare, {.min = 0.0f, .max = 4.0f, .sliderMax = 1.0f});
    visitor.field("Fewest slots", settings.fewestSlots, {.min = 1, .max = 65536, .sliderMax = 1024});
    visitor.field("Chunk slots", settings.chunkSlots, {.min = 1, .max = 4096, .sliderMax = 256});
}

// Divided in double, so a point far out still lands in its cell, and floored, so cells west and north count down.
CellKey worldCellOf(const Vec3& position, float size) {
    const double side = static_cast<double>(size);
    return CellKey{static_cast<std::int64_t>(std::floor(static_cast<double>(position.x) / side)),
                   static_cast<std::int64_t>(std::floor(static_cast<double>(position.z) / side))};
}

std::size_t cellBlockSlots(std::size_t needed, const CellSettings& settings) {
    const double withSpare = std::ceil(static_cast<double>(needed) * (1.0 + static_cast<double>(std::max(settings.spareShare, 0.0f))));
    const std::size_t slots = std::max(static_cast<std::size_t>(withSpare), static_cast<std::size_t>(std::max(settings.fewestSlots, 1)));
    const std::size_t chunk = static_cast<std::size_t>(std::max(settings.chunkSlots, 1));
    return (slots + chunk - 1) / chunk * chunk;
}

// Each step spreads the bits apart: 16 bits to every other place of 32.
std::uint32_t interleaveBits(std::uint32_t x, std::uint32_t z) {
    const auto spread = [](std::uint32_t value) {
        value &= 0xFFFFu;
        value = (value | (value << 8)) & 0x00FF00FFu;
        value = (value | (value << 4)) & 0x0F0F0F0Fu;
        value = (value | (value << 2)) & 0x33333333u;
        value = (value | (value << 1)) & 0x55555555u;
        return value;
    };
    return spread(x) | (spread(z) << 1);
}

// The cell's corner in double, so a cell far out still places its points to a sixteenth of a millimetre a metre.
std::uint32_t cellMortonCode(const Vec3& position, const CellKey& cell, float size) {
    const double side = static_cast<double>(size);
    const auto across = [side](double coordinate, std::int64_t index) {
        const double share = (coordinate - static_cast<double>(index) * side) / side;
        return static_cast<std::uint32_t>(std::clamp(share * 65536.0, 0.0, 65535.0));
    };
    return interleaveBits(across(static_cast<double>(position.x), cell.x), across(static_cast<double>(position.z), cell.z));
}

// First fit, lowest first: a run's remainder stays free where it was.
std::size_t SlotBlocks::take(std::size_t count) {
    if (count == 0) {
        throw std::invalid_argument("A run of slots holds at least one");
    }

    for (auto run = free.begin(); run != free.end(); ++run) {
        if (run->count >= count) {
            const std::size_t first = run->first;
            run->first += count;
            run->count -= count;

            if (run->count == 0) {
                free.erase(run);
            }

            return first;
        }
    }

    const std::size_t first = last;
    last += count;
    return first;
}

// Placed by its first slot, joined with the runs either side when they touch, and the end pulled back past free runs.
void SlotBlocks::giveBack(std::size_t first, std::size_t count) {
    if (count == 0 || first + count > last) {
        throw std::invalid_argument("Only runs taken can be given back");
    }

    auto after = std::lower_bound(free.begin(), free.end(), first, [](const Run& run, std::size_t slot) { return run.first < slot; });
    after = free.insert(after, Run{first, count});

    if (after + 1 != free.end() && after->first + after->count == (after + 1)->first) {
        after->count += (after + 1)->count;
        free.erase(after + 1);
    }

    if (after != free.begin() && (after - 1)->first + (after - 1)->count == after->first) {
        (after - 1)->count += after->count;
        after = free.erase(after) - 1;
    }

    if (!free.empty() && free.back().first + free.back().count == last) {
        last = free.back().first;
        free.pop_back();
    }
}

std::size_t SlotBlocks::end() const {
    return last;
}

void SlotBlocks::clear() {
    free.clear();
    last = 0;
}
