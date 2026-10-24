#include "assets/gltf_loader.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

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
    * a normal map with its scale, an occlusion texture with its strength, an emissive texture, and an emissive colour
    * made brighter by KHR_materials_emissive_strength, and glass: transmission, a volume's thickness and attenuation, an
    * index of refraction), a material with no pbrMetallicRoughness block, and no material at all. glTF's defaults for missing values are metallic 1 and roughness 1.
    */
    const char* const document = R"json(
{
    "asset": { "version": "2.0" },
    "extensionsUsed": ["KHR_materials_emissive_strength", "KHR_materials_transmission", "KHR_materials_volume", "KHR_materials_ior"],
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
                "baseColorFactor": [1.0, 1.0, 1.0, 0.8],
                "metallicFactor": 0.25,
                "roughnessFactor": 0.75,
                "metallicRoughnessTexture": { "index": 0 }
            },
            "normalTexture": { "index": 0, "scale": 0.5 },
            "occlusionTexture": { "index": 0, "strength": 0.75 },
            "emissiveTexture": { "index": 0 },
            "alphaMode": "MASK",
            "alphaCutoff": 0.3,
            "doubleSided": true,
            "emissiveFactor": [0.5, 0.25, 0.0],
            "extensions": {
                "KHR_materials_emissive_strength": { "emissiveStrength": 4.0 },
                "KHR_materials_transmission": { "transmissionFactor": 0.6, "transmissionTexture": { "index": 0 } },
                "KHR_materials_volume": { "thicknessFactor": 0.5, "thicknessTexture": { "index": 0 }, "attenuationDistance": 2.0, "attenuationColor": [0.5, 0.8, 1.0] },
                "KHR_materials_ior": { "ior": 1.33 }
            }
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

    // The document with one piece of text swapped for another, written beside the first and loaded.
    Model loadVariant(const std::filesystem::path& directory, const char* name, const std::string& from, const std::string& to) {
        std::string variant = document;
        variant.replace(variant.find(from), from.size(), to);

        {
            std::ofstream output{directory / name};
            output << variant;
        }

        return loadGltf((directory / name).string());
    }

    /*
    * Khronos's alpha test model (Ed Mackey, AGI, CC BY 4.0): an opaque bed, cut-outs at cutoffs 0.25, 0.5 (the default)
    * and 0.75, blended and opaque rows, every one but the bed double-sided. Its nine parts, one per mesh.
    */
    void testAlphaBlendModeModel() {
        const Model model = loadGltf("assets/models/AlphaBlendModeTest.glb");
        assert(model.parts.size() == 9);

        int masked = 0;
        int blended = 0;
        int doubleSided = 0;
        bool cutoffs[3] = {false, false, false};

        for (const ModelPart& part : model.parts) {
            const MaterialSource& material = part.material;
            doubleSided += material.doubleSided ? 1 : 0;

            blended += material.alphaMode == AlphaMode::Blend ? 1 : 0;

            if (material.alphaMode == AlphaMode::Mask) {
                ++masked;
                cutoffs[0] = cutoffs[0] || nearlyEqual(material.alphaCutoff, 0.25f);
                cutoffs[1] = cutoffs[1] || nearlyEqual(material.alphaCutoff, 0.5f);
                cutoffs[2] = cutoffs[2] || nearlyEqual(material.alphaCutoff, 0.75f);
            }
        }

        assert(masked == 3 && cutoffs[0] && cutoffs[1] && cutoffs[2]);
        assert(blended == 2);
        assert(doubleSided == 8);
    }

    bool throwsWhenLoaded(const std::filesystem::path& directory, const char* name, const std::string& from, const std::string& to) {
        try {
            loadVariant(directory, name, from, to);
        }
        catch (const std::runtime_error&) {
            return true;
        }

        return false;
    }
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
        assert(full.normalTexture.path == full.metallicRoughnessTexture.path);
        assert(nearlyEqual(full.normalScale, 0.5f));
        assert(full.occlusionTexture.path == full.metallicRoughnessTexture.path);
        assert(nearlyEqual(full.occlusionStrength, 0.75f));
        assert(full.emissiveTexture.path == full.metallicRoughnessTexture.path);
        assert(full.alphaMode == AlphaMode::Mask);
        assert(nearlyEqual(full.alphaCutoff, 0.3f));
        assert(full.doubleSided);
        assert(nearlyEqual(full.opacity, 0.8f));
        assert(nearlyEqual(full.transmission, 0.6f));
        assert(full.transmissionTexture.path == full.metallicRoughnessTexture.path);
        assert(nearlyEqual(full.thickness, 0.5f) && full.thicknessTexture.path == full.metallicRoughnessTexture.path);
        assert(nearlyEqual(full.attenuationDistance, 2.0f) && nearlyEqual(full.attenuationColour.y, 0.8f) && nearlyEqual(full.ior, 1.33f));

        const MaterialSource& withoutFactors = model.parts[1].material;
        assert(nearlyEqual(withoutFactors.metallic, 1.0f));
        assert(nearlyEqual(withoutFactors.roughness, 1.0f));
        assert(!withoutFactors.hasTexture());
        assert(!withoutFactors.normalTexture.isSet());
        assert(nearlyEqual(withoutFactors.normalScale, 1.0f));
        assert(!withoutFactors.occlusionTexture.isSet());
        assert(nearlyEqual(withoutFactors.occlusionStrength, 1.0f));
        assert(!withoutFactors.emissiveTexture.isSet());
        assert(withoutFactors.alphaMode == AlphaMode::Opaque);
        assert(nearlyEqual(withoutFactors.alphaCutoff, 0.5f));
        assert(!withoutFactors.doubleSided);
        assert(nearlyEqual(withoutFactors.opacity, 1.0f));
        assert(nearlyEqual(withoutFactors.transmission, 0.0f));
        assert(!withoutFactors.transmissionTexture.isSet());
        assert(withoutFactors.thickness == 0.0f && !withoutFactors.thicknessTexture.isSet() && nearlyEqual(withoutFactors.ior, 1.5f));
        assert(std::isinf(withoutFactors.attenuationDistance) && withoutFactors.attenuationColour.x == 1.0f);

        const MaterialSource& withoutMaterial = model.parts[2].material;
        assert(nearlyEqual(withoutMaterial.metallic, 1.0f));
        assert(nearlyEqual(withoutMaterial.roughness, 1.0f));
        assert(withoutMaterial.colour.r == 255);

        // glTF's occlusion strength is a share, 0..1, like the factors, and a cutoff is at least 0.
        assert(throwsWhenLoaded(directory, "too_strong.gltf", "\"strength\": 0.75", "\"strength\": 1.5"));
        assert(throwsWhenLoaded(directory, "negative_cutoff.gltf", "\"alphaCutoff\": 0.3", "\"alphaCutoff\": -0.1"));

        // Blended materials (glass, decals) are drawn after the opaque scene.
        const Model blended = loadVariant(directory, "blended.gltf", "\"alphaMode\": \"MASK\"", "\"alphaMode\": \"BLEND\"");
        assert(blended.parts[0].material.alphaMode == AlphaMode::Blend);
        assert(throwsWhenLoaded(directory, "too_transmissive.gltf", "\"transmissionFactor\": 0.6", "\"transmissionFactor\": 1.5"));

        // Thick glass: an index below 1, a negative thickness, no attenuation distance or a colour past 0..1 are refused.
        assert(throwsWhenLoaded(directory, "low_ior.gltf", "\"ior\": 1.33", "\"ior\": 0.5"));
        assert(throwsWhenLoaded(directory, "negative_thickness.gltf", "\"thicknessFactor\": 0.5", "\"thicknessFactor\": -1.0"));
        assert(throwsWhenLoaded(directory, "zero_attenuation.gltf", "\"attenuationDistance\": 2.0", "\"attenuationDistance\": 0.0"));
        assert(throwsWhenLoaded(directory, "bright_attenuation.gltf", "[0.5, 0.8, 1.0]", "[0.5, 0.8, 1.5]"));
    }
    catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }

    std::filesystem::remove_all(directory);

    testAlphaBlendModeModel();
}
