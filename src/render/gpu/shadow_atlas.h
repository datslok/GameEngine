#pragma once

#include "ecs/entity.h"

#include <cstdint>
#include <optional>
#include <vector>

/*
* Space management for the shadow atlas, kept from frame to frame so a light's tiles stay where they are and what was
* drawn into them can be reused (shadow caching).
*/

// A square of the atlas, in pixels from its top-left corner.
struct AtlasSquare {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t size = 0;

    bool operator==(const AtlasSquare& other) const = default;
};

/*
* Hands out power-of-two squares from 256 to 1024 pixels (a "buddy" allocator on a quadtree). The atlas starts as a grid
* of 1024 squares; a request takes the smallest free square that fits, splitting a bigger one into quarters as needed.
* A released square joins its three siblings again once all four are free, so the atlas does not crumble into pieces.
*/
class ShadowAtlasAllocator {
public:
    ShadowAtlasAllocator();

    // Nothing when no free square is big enough, even if enough small ones are free.
    std::optional<AtlasSquare> allocate(std::uint32_t size);
    void release(const AtlasSquare& square);

    // Everything free again.
    void reset();

    std::uint64_t freeTexels() const;

private:
    std::vector<AtlasSquare> freeSquares;
};

// Which view a tile is for: the kind of light (0 directional, 1 spot, 2 point), whose light it is, and a number within
// it (a point light's face). Lights without an entity are told apart by their slot instead.
struct ShadowTileKey {
    std::uint32_t kind = 0;
    Entity entity{};
    std::uint32_t index = 0;

    bool operator==(const ShadowTileKey& other) const = default;
};

struct ShadowSquareRequest {
    ShadowTileKey key;
    std::uint32_t size = 0;
};

/*
* The atlas over time: each frame the views that need tiles ask again, and those asking for the same size as last frame
* keep their squares. Views no longer asked for give theirs back, and new ones are placed largest first. If scattered
* free space leaves a new square no room, everything is packed again from scratch, which always fits when the total
* area does (every tile is then redrawn once).
*/
class ShadowAtlasLayout {
public:
    // One square per request, in the same order.
    std::vector<AtlasSquare> place(const std::vector<ShadowSquareRequest>& requests);

    // Whether the last place() had to start again from an empty atlas.
    bool repackedLastTime() const;

private:
    struct Held {
        ShadowTileKey key;
        AtlasSquare square;
    };

    ShadowAtlasAllocator allocator;
    std::vector<Held> held;
    bool repacked = false;

    // Places the given requests largest first; false if one did not fit.
    bool placeLargestFirst(const std::vector<ShadowSquareRequest>& requests, const std::vector<std::size_t>& indices,
                           std::vector<AtlasSquare>& squares);
};
