#include "render/gpu/shadow_cache.h"

#include <algorithm>
#include <cstring>

namespace {
    // FNV-1a: a simple, well-spread hash, fed one byte at a time.
    constexpr std::uint64_t fnvOffset = 14695981039346656037ull;
    constexpr std::uint64_t fnvPrime = 1099511628211ull;

    void mix(std::uint64_t& hash, const void* data, std::size_t byteCount) {
        const unsigned char* bytes = static_cast<const unsigned char*>(data);

        for (std::size_t i = 0; i < byteCount; ++i) {
            hash = (hash ^ bytes[i]) * fnvPrime;
        }
    }

    bool sameMatrix(const Mat4& a, const Mat4& b) {
        return std::memcmp(a.values, b.values, sizeof(a.values)) == 0;
    }

    bool overlap(const ShadowTile& a, const ShadowTile& b) {
        return a.x < b.x + b.size && b.x < a.x + a.size && a.y < b.y + b.size && b.y < a.y + a.size;
    }
}

/*
* The shadow pass draws only positions, so only the mesh and where it is matter, not its material. The count goes in
* too, so a list cannot match a longer one that starts the same way.
*/
std::uint64_t shadowCasterSignature(const std::vector<DrawItem>& draws, const std::vector<std::size_t>& casters) {
    std::uint64_t hash = fnvOffset;
    const std::uint64_t count = casters.size();
    mix(hash, &count, sizeof(count));

    for (std::size_t index : casters) {
        const DrawItem& draw = draws[index];
        mix(hash, &draw.mesh.index, sizeof(draw.mesh.index));
        mix(hash, draw.model.values, sizeof(draw.model.values));
    }

    return hash;
}

/*
* The matrix is compared exactly: a light that moved by any amount sees a different image. Lights standing still are
* given the same matrix every frame, so they match.
*/
bool ShadowTileMemory::isCurrent(const ShadowTile& tile, std::uint64_t signature) const {
    return std::any_of(entries.begin(), entries.end(), [&](const Entry& entry) {
        return entry.tile.x == tile.x && entry.tile.y == tile.y && entry.tile.size == tile.size &&
               entry.signature == signature && sameMatrix(entry.tile.matrix, tile.matrix);
    });
}

void ShadowTileMemory::remember(const ShadowTile& tile, std::uint64_t signature) {
    std::erase_if(entries, [&](const Entry& entry) {
        return overlap(entry.tile, tile);
    });

    entries.push_back(Entry{tile, signature});
}

void ShadowTileMemory::forgetAll() {
    entries.clear();
}
