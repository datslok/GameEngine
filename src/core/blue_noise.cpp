#include "core/blue_noise.h"
#include "core/hash.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace {
    // The Gaussian's spread (texels): Ulichney's 1.5.
    constexpr double energySigma = 1.5;

    // The share of the tile the starting pattern sets.
    constexpr int startingShare = 10;

    /*
    * A tile's set texels and the energy their Gaussians spread over the torus. Adding or removing a texel adds or
    * subtracts its kernel everywhere (the kernel is tabled by offset, so each update is one pass over the tile); the
    * tightest cluster is the set texel of most energy, the largest void the unset one of least, ties to the lowest index
    * so the result never depends on anything but the seed.
    */
    class Pattern {
    public:
        explicit Pattern(int size) : size(size), set(static_cast<std::size_t>(size * size), false), energy(static_cast<std::size_t>(size * size), 0.0) {
            kernel.resize(static_cast<std::size_t>(size * size));

            for (int dy = 0; dy < size; ++dy) {
                for (int dx = 0; dx < size; ++dx) {
                    const int wrappedX = std::min(dx, size - dx);
                    const int wrappedY = std::min(dy, size - dy);
                    kernel[static_cast<std::size_t>(dy * size + dx)] = std::exp(-(wrappedX * wrappedX + wrappedY * wrappedY) / (2.0 * energySigma * energySigma));
                }
            }
        }

        bool isSet(int texel) const {
            return set[static_cast<std::size_t>(texel)];
        }

        void flip(int texel, bool on) {
            set[static_cast<std::size_t>(texel)] = on;
            const double sign = on ? 1.0 : -1.0;
            const int tx = texel % size;
            const int ty = texel / size;

            for (int y = 0; y < size; ++y) {
                const int dy = (y - ty + size) % size;

                for (int x = 0; x < size; ++x) {
                    const int dx = (x - tx + size) % size;
                    energy[static_cast<std::size_t>(y * size + x)] += sign * kernel[static_cast<std::size_t>(dy * size + dx)];
                }
            }
        }

        // The set texel of most energy (wanted set) or the unset one of least (wanted unset).
        int find(bool wantedSet, bool most) const {
            int best = -1;

            for (int texel = 0; texel < size * size; ++texel) {
                if (isSet(texel) != wantedSet) {
                    continue;
                }

                const double value = energy[static_cast<std::size_t>(texel)];

                if (best < 0 || (most ? value > energy[static_cast<std::size_t>(best)] : value < energy[static_cast<std::size_t>(best)])) {
                    best = texel;
                }
            }

            return best;
        }

    private:
        int size;
        std::vector<bool> set;
        std::vector<double> energy;
        std::vector<double> kernel;
    };
}

std::vector<float> makeBlueNoise(int size, std::uint32_t seed) {
    if (size < 4) {
        throw std::invalid_argument("Blue noise needs a tile of at least 4 texels a side");
    }

    const int texels = size * size;
    const int starting = std::max(texels / startingShare, 1);

    // A random tenth of the texels: a shuffle driven by the seed.
    std::vector<int> order(static_cast<std::size_t>(texels));
    std::iota(order.begin(), order.end(), 0);
    std::uint64_t state = mixBits(seed + 0x9e3779b97f4a7c15ull);

    for (int i = texels - 1; i > 0; --i) {
        state = mixBits(state + 0x9e3779b97f4a7c15ull);
        std::swap(order[static_cast<std::size_t>(i)], order[static_cast<std::size_t>(state % static_cast<std::uint64_t>(i + 1))]);
    }

    Pattern prototype(size);

    for (int i = 0; i < starting; ++i) {
        prototype.flip(order[static_cast<std::size_t>(i)], true);
    }

    // Made even: the tightest cluster's texel moves to the largest void until it would land where it was.
    for (int guard = 0; guard < texels * 4; ++guard) {
        const int cluster = prototype.find(true, true);
        prototype.flip(cluster, false);
        const int voidTexel = prototype.find(false, false);

        if (voidTexel == cluster) {
            prototype.flip(cluster, true);
            break;
        }

        prototype.flip(voidTexel, true);
    }

    std::vector<int> rank(static_cast<std::size_t>(texels), -1);

    // Ranks below the start's count: the start's texels taken away, tightest cluster first, counting down.
    Pattern removing = prototype;

    for (int r = starting - 1; r >= 0; --r) {
        const int cluster = removing.find(true, true);
        rank[static_cast<std::size_t>(cluster)] = r;
        removing.flip(cluster, false);
    }

    // Up to half: texels added into the largest void.
    Pattern adding = prototype;
    int next = starting;

    for (; next < texels / 2; ++next) {
        const int voidTexel = adding.find(false, false);
        rank[static_cast<std::size_t>(voidTexel)] = next;
        adding.flip(voidTexel, true);
    }

    // Past half the unset texels are the minority: their own energy, their tightest cluster set next.
    Pattern unset(size);

    for (int texel = 0; texel < texels; ++texel) {
        if (!adding.isSet(texel)) {
            unset.flip(texel, true);
        }
    }

    for (; next < texels; ++next) {
        const int cluster = unset.find(true, true);
        rank[static_cast<std::size_t>(cluster)] = next;
        unset.flip(cluster, false);
    }

    std::vector<float> values(static_cast<std::size_t>(texels));

    for (int texel = 0; texel < texels; ++texel) {
        values[static_cast<std::size_t>(texel)] = (static_cast<float>(rank[static_cast<std::size_t>(texel)]) + 0.5f) / static_cast<float>(texels);
    }

    return values;
}
