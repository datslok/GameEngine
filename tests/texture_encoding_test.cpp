#include "assets/asset_manager.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
    const char* const imagePath = "assets/textures/demo.png";

    std::shared_ptr<const std::vector<std::uint8_t>> readImageBytes() {
        std::ifstream file{imagePath, std::ios::binary};
        assert(file);
        return std::make_shared<const std::vector<std::uint8_t>>(std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{});
    }

    /*
    * Colour images are stored sRGB-encoded and decoded by the GPU; data images (roughness, metallic, later normals) are
    * plain numbers and must not be. The same file asked for both ways is two textures, each cached on its own.
    */
    void testEncodingIsPartOfTheCacheKey() {
        AssetManager assets;

        const TextureHandle colour = assets.loadTexture(imagePath);
        const TextureHandle data = assets.loadTexture(imagePath, true, TextureEncoding::Linear);

        assert(!(colour == data));
        assert(assets.getTexture(colour).encoding == TextureEncoding::Srgb);
        assert(assets.getTexture(data).encoding == TextureEncoding::Linear);
        assert(assets.getTexture(colour).pixels == assets.getTexture(data).pixels);
        assert(assets.loadTexture(imagePath, true, TextureEncoding::Linear) == data);
        assert(assets.getTextureCount() == 2);

        const auto bytes = readImageBytes();
        const TextureHandle embeddedColour = assets.loadTexture(bytes, false);
        const TextureHandle embeddedData = assets.loadTexture(bytes, false, TextureEncoding::Linear);

        assert(!(embeddedColour == embeddedData));
        assert(assets.getTexture(embeddedData).encoding == TextureEncoding::Linear);
        assert(assets.loadTexture(bytes, false, TextureEncoding::Linear) == embeddedData);
        assert(assets.getTextureCount() == 4);
    }

    // Textures made in code are colour images unless they say otherwise.
    void testAddedTexturesDefaultToSrgb() {
        AssetManager assets;
        ImageData image;
        image.width = 1;
        image.height = 1;
        image.pixels = {255, 255, 255, 255};

        assert(assets.getTexture(assets.addTexture(image)).encoding == TextureEncoding::Srgb);

        image.encoding = TextureEncoding::Linear;
        assert(assets.getTexture(assets.addTexture(image)).encoding == TextureEncoding::Linear);
    }

    // A material's factors arrive as they are; its colour texture is sRGB, its metallic-roughness and
    // normal textures linear (numbers and directions, not colours).
    void testMakeMaterialReadsThePhysicalProperties() {
        AssetManager assets;

        MaterialSource source;
        source.colour = Pixel{200, 100, 50};
        source.metallic = 0.25f;
        source.roughness = 0.75f;
        source.emissive = Vec3{2.0f, 1.0f, 0.0f};
        source.colourTexture.path = imagePath;
        source.metallicRoughnessTexture.embeddedImage = readImageBytes();
        source.normalTexture.path = imagePath;
        source.normalScale = 0.6f;
        source.flipTextureVertically = false;

        const Material material = assets.makeMaterial(source);

        assert(material.colour.r == 200 && material.colour.g == 100 && material.colour.b == 50);
        assert(material.metallic == 0.25f);
        assert(material.roughness == 0.75f);
        assert(material.emissive.x == 2.0f && material.emissive.y == 1.0f && material.emissive.z == 0.0f);

        assert(material.texture.isValid());
        assert(material.metallicRoughnessTexture.isValid());
        assert(assets.getTexture(material.texture).encoding == TextureEncoding::Srgb);
        assert(assets.getTexture(material.metallicRoughnessTexture).encoding == TextureEncoding::Linear);
        assert(assets.getTexture(material.normalTexture).encoding == TextureEncoding::Linear);
        assert(material.normalScale == 0.6f);

        // The same flip as the colour texture: both are read with the same UVs.
        assert(material.texture == assets.loadTexture(imagePath, false));
    }

    // A source without textures or factors gives the default material; a texture cannot be a file and bytes at once.
    void testMaterialSourceDefaultsMatchMaterial() {
        AssetManager assets;
        const Material material = assets.makeMaterial(MaterialSource{});
        const Material defaults;

        assert(material.metallic == defaults.metallic);
        assert(material.roughness == defaults.roughness);
        assert(!material.texture.isValid());
        assert(!material.metallicRoughnessTexture.isValid());
        assert(!material.normalTexture.isValid());
        assert(material.normalScale == defaults.normalScale);
        assert(!MaterialSource{}.hasTexture());

        MaterialSource both;
        both.metallicRoughnessTexture.path = imagePath;
        both.metallicRoughnessTexture.embeddedImage = readImageBytes();
        assert(both.hasTexture());

        bool threw = false;

        try {
            assets.makeMaterial(both);
        }
        catch (const std::invalid_argument&) {
            threw = true;
        }

        assert(threw);
    }
}

void testTextureEncoding() {
    testEncodingIsPartOfTheCacheKey();
    testAddedTexturesDefaultToSrgb();
    testMakeMaterialReadsThePhysicalProperties();
    testMaterialSourceDefaultsMatchMaterial();
}
