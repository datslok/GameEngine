#pragma once

#include <cstdint>
#include <vector>

/*
* Small parts (a twig's leaves) pruned level by level (docs/research/LEAFGEOMETRY.md, Keeping the crowns from going bare):
* each coarser level keeps half the parts of the one below, chosen by a fixed rank so a level's parts are always a subset
* of the finer level's (a part never comes back as the view moves away), and the survivors grow to keep the area (Cook
* and Halstead's stochastic pruning). The rank is bit-reversed order (stratified sampling): number the parts along their
* stem from a seeded offset, reverse each number's binary digits, keep the lowest; so level k keeps every 2^k-th part
* and the gaps stay even, never two neighbours gone at the first halving.
*/

// The parts in the order they survive, the longest kept first: a level keeps a prefix of it (prunedKept). offset turns
// where the numbering starts (any value; taken modulo count), so twigs sharing a count lose different parts.
std::vector<std::uint32_t> prunedOrder(std::uint32_t count, std::uint32_t offset);

// How many parts level keeps (0 the finest, all of them): count / 2^level rounded up, at least 1; 0 for no parts.
std::uint32_t prunedKept(std::uint32_t count, int level);

// How many levels there are, the finest included, until one part is left: 1 for count 0 or 1.
int prunedLevels(std::uint32_t count);
