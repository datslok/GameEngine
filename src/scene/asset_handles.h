#pragma once

#include <cstdint>
#include <limits>

/*
* A small typed number that names an asset owned by the AssetManager, like an atomic number names an element in the periodic table.
* The Tag only makes MeshHandle and TextureHandle different types, so one cannot be passed where the other is expected.
* Handles are array indices: looking one up is a single array access, and the renderer stores its GPU copies with the same numbering.
* A default-constructed handle means "none". Assets are never unloaded yet, so handles cannot go stale and need no generation counter.
*/
template <typename Tag>
struct AssetHandle {
    static constexpr std::uint32_t noneIndex = std::numeric_limits<std::uint32_t>::max();

    std::uint32_t index = noneIndex;

    bool isValid() const {
        return index != noneIndex;
    }

    bool operator==(const AssetHandle& other) const = default;
};

struct MeshTag;
struct TextureTag;

using MeshHandle = AssetHandle<MeshTag>;
using TextureHandle = AssetHandle<TextureTag>;

/*
* How a texture's bytes are meant. Colour images (photos, painted colour maps) are stored sRGB-encoded, which spends more
* of the 256 steps on dark shades where the eye notices them; the GPU decodes them to linear light as it samples.
* Data images (roughness and metallic, later normal maps) hold plain numbers 0..1 and must be read exactly as stored.
*/
enum class TextureEncoding {
    Srgb,
    Linear
};
