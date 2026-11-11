#pragma once

#include <cstdint>
#include <vector>

/*
* Blue noise: a tile of values 0..1, each a different rank, arranged so that nearby texels never clump: its energy sits at
* high spatial frequencies only. Noise like this left over after averaging looks like fine grain, which anti-aliasing
* dissolves, where patterned noise (interleaved gradient noise) leaves shapes that move from frame to frame.
*
* Made by the void-and-cluster method (Ulichney 1993): a tile's texels are ranked by adding them one at a time where the
* pattern so far is emptiest (its largest void, the lowest energy, energy being each set texel's Gaussian spread over the
* torus, sigma 1.5 texels), starting from a well-spread pattern of a tenth of the texels (random points made even by
* moving the tightest cluster's point into the largest void until it lands where it started), ranked downwards by
* removing the tightest cluster; past half the tile the roles swap (the unset texels' own energy, their tightest
* cluster set next). Rank r becomes (r + 0.5) / texels, so the values are spread exactly evenly.
*
* Deterministic: the same size and seed give the same tile, bit for bit.
*/
std::vector<float> makeBlueNoise(int size, std::uint32_t seed);