#include "math/median_split.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <initializer_list>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "test_levels.h"

namespace {
    // The points' groups in an order: each run of groupSize, the last what is left, as the sets of their numbers.
    std::vector<std::set<std::size_t>> groupsOf(const std::vector<std::size_t>& order, std::size_t groupSize) {
        std::vector<std::set<std::size_t>> groups;

        for (std::size_t first = 0; first < order.size(); first += groupSize) {
            groups.emplace_back(order.begin() + static_cast<std::ptrdiff_t>(first),
                                order.begin() + static_cast<std::ptrdiff_t>(std::min(first + groupSize, order.size())));
        }

        return groups;
    }

    // The mean over an order's groups of the farthest point from its group's box centre.
    float meanGroupReach(const std::vector<Vec3>& points, const std::vector<std::size_t>& order, std::size_t groupSize) {
        float total = 0.0f;
        std::size_t groups = 0;

        for (std::size_t first = 0; first < order.size(); first += groupSize) {
            const std::size_t end = std::min(first + groupSize, order.size());
            Vec3 low = points[order[first]];
            Vec3 high = low;

            for (std::size_t place = first; place < end; ++place) {
                const Vec3& point = points[order[place]];
                low = Vec3{std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z)};
                high = Vec3{std::max(high.x, point.x), std::max(high.y, point.y), std::max(high.z, point.z)};
            }

            const Vec3 centre = (low + high) * 0.5f;
            float reach = 0.0f;

            for (std::size_t place = first; place < end; ++place) {
                reach = std::max(reach, (points[order[place]] - centre).length());
            }

            total += reach;
            ++groups;
        }

        return total / static_cast<float>(groups);
    }

    /*
    * Half the groups rounded up go first. Points on a line, given out of order, fall into groups of neighbours: the
    * lower points first, every group but the last full. Equal points split by their numbers, so the order never
    * depends on the sort's. Scattered points in groups far tighter than in their given order, no two groups' boxes
    * overlapping. A group of none refused.
    */
    void testMedianSplitGroupsNeighbours() {
        assert(medianSplitFirstHalf(1) == 1 && medianSplitFirstHalf(2) == 1 && medianSplitFirstHalf(3) == 2 && medianSplitFirstHalf(8) == 4);

        const std::vector<float> places{7.0f, 2.0f, 9.0f, 0.0f, 5.0f, 3.0f, 8.0f, 1.0f, 6.0f, 4.0f};
        std::vector<Vec3> line;

        for (const float place : places) {
            line.push_back(Vec3{place, 0.5f * place, -0.25f * place});
        }

        const std::vector<std::size_t> order = medianSplitOrder(line, 4);
        std::vector<std::size_t> sorted = order;
        std::sort(sorted.begin(), sorted.end());

        for (std::size_t number = 0; number < sorted.size(); ++number) {
            assert(sorted[number] == number);
        }

        const auto byPlace = [&](std::initializer_list<float> wanted) {
            std::set<std::size_t> numbers;

            for (const float place : wanted) {
                numbers.insert(static_cast<std::size_t>(std::find(places.begin(), places.end(), place) - places.begin()));
            }

            return numbers;
        };
        const std::vector<std::set<std::size_t>> groups = groupsOf(order, 4);
        assert(groups.size() == 3);
        assert(groups[0] == byPlace({0.0f, 1.0f, 2.0f, 3.0f}) && groups[1] == byPlace({4.0f, 5.0f, 6.0f, 7.0f}) && groups[2] == byPlace({8.0f, 9.0f}));

        const std::vector<Vec3> same(9, Vec3{1.0f, 2.0f, 3.0f});
        const std::vector<std::set<std::size_t>> tied = groupsOf(medianSplitOrder(same, 3), 3);
        assert(tied.size() == 3 && tied[0] == (std::set<std::size_t>{0, 1, 2}) && tied[1] == (std::set<std::size_t>{3, 4, 5}) &&
               tied[2] == (std::set<std::size_t>{6, 7, 8}));

        std::mt19937 random(11u);
        std::uniform_real_distribution<float> place(-10.0f, 10.0f);
        std::vector<Vec3> scattered;
        std::vector<std::size_t> given;

        for (std::size_t number = 0; number < 3000; ++number) {
            scattered.push_back(Vec3{place(random), place(random) * 0.5f, place(random)});
            given.push_back(number);
        }

        assert(medianSplitOrder(scattered, 12) == medianSplitOrder(scattered, 12));

        // A split plane parts every two groups, so no two groups' boxes overlap (a split off a whole number of groups
        // leaves a group across a plane).
        const std::vector<std::size_t> grouped = medianSplitOrder(scattered, 12);
        std::vector<std::pair<Vec3, Vec3>> boxes;

        for (std::size_t first = 0; first < grouped.size(); first += 12) {
            Vec3 low = scattered[grouped[first]];
            Vec3 high = low;

            for (std::size_t place = first; place < std::min(first + 12, grouped.size()); ++place) {
                const Vec3& point = scattered[grouped[place]];
                low = Vec3{std::min(low.x, point.x), std::min(low.y, point.y), std::min(low.z, point.z)};
                high = Vec3{std::max(high.x, point.x), std::max(high.y, point.y), std::max(high.z, point.z)};
            }

            boxes.emplace_back(low, high);
        }

        for (std::size_t a = 0; a < boxes.size(); ++a) {
            for (std::size_t b = a + 1; b < boxes.size(); ++b) {
                const bool overlap = boxes[a].first.x < boxes[b].second.x && boxes[b].first.x < boxes[a].second.x &&
                                     boxes[a].first.y < boxes[b].second.y && boxes[b].first.y < boxes[a].second.y &&
                                     boxes[a].first.z < boxes[b].second.z && boxes[b].first.z < boxes[a].second.z;
                assert(!overlap);
            }
        }

        assert(meanGroupReach(scattered, medianSplitOrder(scattered, 12), 12) < 0.4f * meanGroupReach(scattered, given, 12));
        assert(medianSplitOrder({}, 12).empty());
        assert(throwsInvalid([&] { medianSplitOrder(line, 0); }));
    }
}

void testMedianSplit() {
    testMedianSplitGroupsNeighbours();
}
