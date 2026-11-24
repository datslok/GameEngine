#pragma once

#include "math/vec3.h"

#include <cstddef>
#include <vector>

// How many of a run of groups the median split puts in its first half: half, rounded up.
std::size_t medianSplitFirstHalf(std::size_t groups);

/*
* The points' numbers in groups of neighbours: while a run holds more than a group, split it along the longest side of
* its points' box at a whole number of groups (medianSplitFirstHalf of them), the lower points first (ties by their
* number, so the order never depends on the sort's), each half again. Each run of groupSize in the order is a group,
* every one but the last full. Throws std::invalid_argument for a group of none. Deterministic; cost grows with the
* points times the split's depth.
*/
std::vector<std::size_t> medianSplitOrder(const std::vector<Vec3>& points, std::size_t groupSize);
