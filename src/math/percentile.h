#pragma once

#include <vector>

/*
* The nearest rank: the value at place ceil(share x count) of the values sorted, counted from 1, so the least value
* that share of them are at or below (share 1 is the largest). 0 for no values. Throws std::invalid_argument for a
* share outside (0, 1]. Linear in the values (a partial sort of the copy).
*/
float nearestRank(std::vector<float> values, float share);
