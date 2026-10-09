#include "render/gpu/shadow_atlas.h"
#include "render/gpu/shadow_map.h"

#include <algorithm>
#include <stdexcept>

ShadowAtlasAllocator::ShadowAtlasAllocator() {
    reset();
}

void ShadowAtlasAllocator::reset() {
    freeSquares.clear();

    for (std::uint32_t y = 0; y < shadowAtlasHeight; y += largestShadowTileSize) {
        for (std::uint32_t x = 0; x < shadowAtlasWidth; x += largestShadowTileSize) {
            freeSquares.push_back(AtlasSquare{x, y, largestShadowTileSize});
        }
    }
}

/*
* The smallest free square that fits keeps big squares whole for big requests. Splitting keeps the top-left quarter
* and frees the other three, until the square is the size asked for.
*/
std::optional<AtlasSquare> ShadowAtlasAllocator::allocate(std::uint32_t size) {
    std::size_t best = freeSquares.size();

    for (std::size_t i = 0; i < freeSquares.size(); ++i) {
        if (freeSquares[i].size >= size && (best == freeSquares.size() || freeSquares[i].size < freeSquares[best].size)) {
            best = i;
        }
    }

    if (best == freeSquares.size()) {
        return std::nullopt;
    }

    AtlasSquare square = freeSquares[best];
    freeSquares.erase(freeSquares.begin() + static_cast<std::ptrdiff_t>(best));

    while (square.size > size) {
        square.size /= 2;
        freeSquares.push_back(AtlasSquare{square.x + square.size, square.y, square.size});
        freeSquares.push_back(AtlasSquare{square.x, square.y + square.size, square.size});
        freeSquares.push_back(AtlasSquare{square.x + square.size, square.y + square.size, square.size});
    }

    return square;
}

/*
* Squares only ever come from splitting, so a square's parent is found by rounding its corner down to the parent's
* size, and its siblings are the parent's other three quarters. When all three are free too, the four become the parent
* again, which may in turn join its own siblings, up to the largest size.
*/
void ShadowAtlasAllocator::release(const AtlasSquare& released) {
    AtlasSquare square = released;

    while (square.size < largestShadowTileSize) {
        const std::uint32_t parentSize = square.size * 2;
        const std::uint32_t parentX = square.x - square.x % parentSize;
        const std::uint32_t parentY = square.y - square.y % parentSize;

        std::vector<std::size_t> siblings;

        for (const AtlasSquare& quarter : {
                 AtlasSquare{parentX, parentY, square.size},
                 AtlasSquare{parentX + square.size, parentY, square.size},
                 AtlasSquare{parentX, parentY + square.size, square.size},
                 AtlasSquare{parentX + square.size, parentY + square.size, square.size}}) {
            if (quarter == square) {
                continue;
            }

            const auto found = std::find(freeSquares.begin(), freeSquares.end(), quarter);

            if (found == freeSquares.end()) {
                break;
            }

            siblings.push_back(static_cast<std::size_t>(found - freeSquares.begin()));
        }

        if (siblings.size() < 3) {
            break;
        }

        // Erase from the back so the earlier positions stay valid.
        std::sort(siblings.rbegin(), siblings.rend());

        for (std::size_t index : siblings) {
            freeSquares.erase(freeSquares.begin() + static_cast<std::ptrdiff_t>(index));
        }

        square = AtlasSquare{parentX, parentY, parentSize};
    }

    freeSquares.push_back(square);
}

std::uint64_t ShadowAtlasAllocator::freeTexels() const {
    std::uint64_t total = 0;

    for (const AtlasSquare& square : freeSquares) {
        total += static_cast<std::uint64_t>(square.size) * square.size;
    }

    return total;
}

/*
* Biggest first, ties in request order: then the quarters of a split square are used up before another is split, so
* on an empty atlas everything fits whenever the total area does.
*/
bool ShadowAtlasLayout::placeLargestFirst(const std::vector<ShadowSquareRequest>& requests, const std::vector<std::size_t>& indices,
                                          std::vector<AtlasSquare>& squares) {
    std::vector<std::size_t> order = indices;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return requests[a].size > requests[b].size;
    });

    for (std::size_t index : order) {
        const std::optional<AtlasSquare> square = allocator.allocate(requests[index].size);

        if (!square) {
            return false;
        }

        squares[index] = *square;
    }

    return true;
}

/*
* First the views asked for again at the same size keep their squares; then the squares nobody asked for are given
* back (before placing, so new views can use them); then the new views are placed. Only if one does not fit is the
* atlas emptied and everything placed afresh.
*/
std::vector<AtlasSquare> ShadowAtlasLayout::place(const std::vector<ShadowSquareRequest>& requests) {
    std::vector<AtlasSquare> squares(requests.size());
    std::vector<bool> placed(requests.size(), false);

    for (const Held& previous : held) {
        bool kept = false;

        for (std::size_t i = 0; i < requests.size(); ++i) {
            if (!placed[i] && requests[i].key == previous.key && requests[i].size == previous.square.size) {
                squares[i] = previous.square;
                placed[i] = true;
                kept = true;
                break;
            }
        }

        if (!kept) {
            allocator.release(previous.square);
        }
    }

    std::vector<std::size_t> newcomers;

    for (std::size_t i = 0; i < requests.size(); ++i) {
        if (!placed[i]) {
            newcomers.push_back(i);
        }
    }

    repacked = false;

    if (!placeLargestFirst(requests, newcomers, squares)) {
        std::vector<std::size_t> everything(requests.size());

        for (std::size_t i = 0; i < everything.size(); ++i) {
            everything[i] = i;
        }

        allocator.reset();
        repacked = true;

        if (!placeLargestFirst(requests, everything, squares)) {
            throw std::logic_error("Shadow tiles do not fit in the atlas");
        }
    }

    held.clear();

    for (std::size_t i = 0; i < requests.size(); ++i) {
        held.push_back(Held{requests[i].key, squares[i]});
    }

    return squares;
}

bool ShadowAtlasLayout::repackedLastTime() const {
    return repacked;
}
