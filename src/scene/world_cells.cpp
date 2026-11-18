#include "scene/world_cells.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

void visit(CellSettings& settings, SettingsVisitor& visitor) {
    visitor.field("Size", settings.size, {.min = 1.0f, .max = 100000.0f, .sliderMin = 16.0f, .sliderMax = 1024.0f, .unit = "m"});
    visitor.field("Spare share", settings.spareShare, {.min = 0.0f, .max = 4.0f, .sliderMax = 1.0f});
    visitor.field("Fewest slots", settings.fewestSlots, {.min = 1, .max = 65536, .sliderMax = 1024});
}

// Divided in double, so a point far out still lands in its cell, and floored, so cells west and north count down.
CellKey worldCellOf(const Vec3& position, float size) {
    const double side = static_cast<double>(size);
    return CellKey{static_cast<std::int64_t>(std::floor(static_cast<double>(position.x) / side)),
                   static_cast<std::int64_t>(std::floor(static_cast<double>(position.z) / side))};
}

std::size_t cellBlockSlots(std::size_t needed, const CellSettings& settings) {
    const double withSpare = std::ceil(static_cast<double>(needed) * (1.0 + static_cast<double>(std::max(settings.spareShare, 0.0f))));
    return std::max(static_cast<std::size_t>(withSpare), static_cast<std::size_t>(std::max(settings.fewestSlots, 1)));
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
