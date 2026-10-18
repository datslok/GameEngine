#include "assets/gltf_loader.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
    void writeFloat(std::ostream& output, float value) {
        const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
        const unsigned char bytes[4] = {
            static_cast<unsigned char>(bits & 0xFF),
            static_cast<unsigned char>((bits >> 8) & 0xFF),
            static_cast<unsigned char>((bits >> 16) & 0xFF),
            static_cast<unsigned char>((bits >> 24) & 0xFF)
        };
        output.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
    }

    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }

    /*
    * Three primitives sharing one triangle: a full metallic-roughness material (factors, a metallic-roughness texture,
    * an emissive colour made brighter by KHR_materials_emissive_strength), a material with no pbrMetallicRoughness block,
    * and no material at all. glTF's defaults for missing values are metallic 1 and roughness 1.
    */
    const char* const document = R"json(
{
    "asset": { "version": "2.0" },
    "extensionsUsed": ["KHR_materials_emissive_strength"],
    "scene": 0,
    "scenes": [{ "nodes": [0] }],
    "nodes": [{ "mesh": 0 }],
    "meshes": [{
        "primitives": [
            { "attributes": { "POSITION": 0, "TEXCOORD_0": 1 }, "material": 0 },
            { "attributes": { "POSITION": 0 }, "material": 1 },
            { "attributes": { "POSITION": 0 } }
        ]
    }],
    "materials": [
        {
            "pbrMetallicRoughness": {
                "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
                "metallicFactor": 0.25,
                "roughnessFactor": 0.75,
                "metallicRoughnessTexture": { "index": 0 }
            },
            "emissiveFactor": [0.5, 0.25, 0.0],
            "extensions": { "KHR_materials_emissive_strength": { "emissiveStrength": 4.0 } }
        },
        { "emissiveFactor": [0.0, 0.0, 0.0] }
    ],
    "textures": [{ "source": 0 }],
    "images": [{ "uri": "surface.png" }],
    "buffers": [{ "uri": "triangle.bin", "byteLength": 60 }],
    "bufferViews": [
        { "buffer": 0, "byteOffset": 0, "byteLength": 36 },
        { "buffer": 0, "byteOffset": 36, "byteLength": 24 }
    ],
    "accessors": [
        { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0] },
        { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC2" }
    ]
}
)json";
}

void testGltfMaterials() {
    const std::filesystem::path directory = std::filesystem::temp_directory_path() / "game_engine_gltf_material_test";

    // Left behind only by a failed run of this test, so it is safe to clear.
    std::filesystem::remove_all(directory);
    std::filesystem::create_directory(directory);

    try {
        {
            std::ofstream output{directory / "triangle.bin", std::ios::binary};
            const float values[] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 1};

            for (float value : values) {
                writeFloat(output, value);
            }
        }

        {
            std::ofstream output{directory / "surface.gltf"};
            output << document;
        }

        std::filesystem::copy_file("assets/textures/demo.png", directory / "surface.png");

        const Model model = loadGltf((directory / "surface.gltf").string());
        assert(model.parts.size() == 3);

        const MaterialSource& full = model.parts[0].material;
        assert(nearlyEqual(full.metallic, 0.25f));
        assert(nearlyEqual(full.roughness, 0.75f));
        assert(nearlyEqual(full.emissive.x, 2.0f));
        assert(nearlyEqual(full.emissive.y, 1.0f));
        assert(nearlyEqual(full.emissive.z, 0.0f));
        assert(!full.colourTexture.isSet());
        assert(full.metallicRoughnessTexture.path == (directory / "surface.png").lexically_normal().generic_string());
        assert(!full.flipTextureVertically);
        assert(full.hasTexture());

        const MaterialSource& withoutFactors = model.parts[1].material;
        assert(nearlyEqual(withoutFactors.metallic, 1.0f));
        assert(nearlyEqual(withoutFactors.roughness, 1.0f));
        assert(!withoutFactors.hasTexture());

        const MaterialSource& withoutMaterial = model.parts[2].material;
        assert(nearlyEqual(withoutMaterial.metallic, 1.0f));
        assert(nearlyEqual(withoutMaterial.roughness, 1.0f));
        assert(withoutMaterial.colour.r == 255);
    }
    catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }

    std::filesystem::remove_all(directory);
}
