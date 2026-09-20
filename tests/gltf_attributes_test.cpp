#include "gltf_loader.h"

#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace {
    // Write a float in glTF's little-endian binary format.
    void writeFloat(std::ostream& output, float value) {
        const std::uint32_t bits =
            std::bit_cast<std::uint32_t>(value);

        const unsigned char bytes[4] = {
            static_cast<unsigned char>(bits & 0xFF),
            static_cast<unsigned char>((bits >> 8) & 0xFF),
            static_cast<unsigned char>((bits >> 16) & 0xFF),
            static_cast<unsigned char>((bits >> 24) & 0xFF)
        };

        output.write(
            reinterpret_cast<const char*>(bytes),
            sizeof(bytes)
        );
    }

    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }
}

void testGltfAttributes() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        "game_engine_gltf_attributes_test";

    if (!std::filesystem::create_directory(directory)) {
        throw std::runtime_error(
            "glTF attribute test directory already exists: " +
            directory.string()
        );
    }

    try {
        // Positions: 36 bytes.
        // Normals:   36 bytes.
        // UVs:       24 bytes.
        const float attributes[] = {
            // Positions.
            0.0f, 0.0f, 0.0f,
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,

            // Distinct unit normals for each vertex.
            1.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 1.0f,

            // Distinct UVs, including values outside [0, 1].
            0.25f,  0.5f,
            1.25f, -0.5f,
            0.0f,   1.0f
        };

        // Indices: 6 bytes. Deliberately use the order 2, 0, 1.
        const unsigned char indices[] = {
            0x02, 0x00,
            0x00, 0x00,
            0x01, 0x00
        };

        {
            std::ofstream output{
                directory / "attributes.bin",
                std::ios::binary
            };

            for (float value : attributes) {
                writeFloat(output, value);
            }

            output.write(
                reinterpret_cast<const char*>(indices),
                sizeof(indices)
            );

            output.close();

            if (!output) {
                throw std::runtime_error(
                    "Could not write attribute test buffer"
                );
            }
        }

        const std::filesystem::path filename =
            directory / "attributes.gltf";

        {
            std::ofstream output{filename};

            output << R"json(
{
    "asset": { "version": "2.0" },
    "scene": 0,
    "scenes": [
        { "nodes": [0] }
    ],
    "nodes": [
        { "mesh": 0 }
    ],
    "meshes": [
        {
            "primitives": [
                {
                    "attributes": {
                        "POSITION": 0,
                        "NORMAL": 1,
                        "TEXCOORD_0": 2
                    },
                    "indices": 3,
                    "mode": 4
                },
                {
                    "attributes": { "POSITION": 0 },
                    "indices": 3,
                    "mode": 4
                }
            ]
        }
    ],
    "buffers": [
        {
            "uri": "attributes.bin",
            "byteLength": 102
        }
    ],
    "bufferViews": [
        { "buffer": 0, "byteOffset": 0,  "byteLength": 36 },
        { "buffer": 0, "byteOffset": 36, "byteLength": 36 },
        { "buffer": 0, "byteOffset": 72, "byteLength": 24 },
        { "buffer": 0, "byteOffset": 96, "byteLength": 6 }
    ],
    "accessors": [
        {
            "bufferView": 0,
            "componentType": 5126,
            "count": 3,
            "type": "VEC3",
            "min": [0, 0, 0],
            "max": [1, 1, 0]
        },
        {
            "bufferView": 1,
            "componentType": 5126,
            "count": 3,
            "type": "VEC3"
        },
        {
            "bufferView": 2,
            "componentType": 5126,
            "count": 3,
            "type": "VEC2"
        },
        {
            "bufferView": 3,
            "componentType": 5123,
            "count": 3,
            "type": "SCALAR"
        }
    ]
}
)json";

            output.close();

            if (!output) {
                throw std::runtime_error(
                    "Could not write attribute test document"
                );
            }
        }

        const Model model = loadGltf(filename.string());

        // Each primitive becomes a separate model part.
        assert(model.parts.size() == 2);
        assert(model.parts[0].mesh != nullptr);
        assert(model.parts[1].mesh != nullptr);

        const Mesh& attributedMesh = *model.parts[0].mesh;
        assert(attributedMesh.triangles.size() == 1);

        const Triangle& triangle = attributedMesh.triangles[0];

        assert(triangle.first == 2);
        assert(triangle.second == 0);
        assert(triangle.third == 1);

        // Expected values in triangle-corner order: 2, 0, 1.
        const Vec3 expectedNormals[] = {
            Vec3{0.0f, 0.0f, 1.0f},
            Vec3{1.0f, 0.0f, 0.0f},
            Vec3{0.0f, 1.0f, 0.0f}
        };

        const Vec2 expectedUvs[] = {
            Vec2{0.0f,   1.0f},
            Vec2{0.25f,  0.5f},
            Vec2{1.25f, -0.5f}
        };

        for (std::size_t corner = 0; corner < 3; ++corner) {
            assert(triangle.normals[corner].has_value());
            assert(triangle.uvs[corner].has_value());

            const Vec3& normal = triangle.normals[corner].value();
            const Vec2& uv = triangle.uvs[corner].value();

            assert(nearlyEqual(normal.x, expectedNormals[corner].x));
            assert(nearlyEqual(normal.y, expectedNormals[corner].y));
            assert(nearlyEqual(normal.z, expectedNormals[corner].z));

            assert(nearlyEqual(uv.x, expectedUvs[corner].x));
            assert(nearlyEqual(uv.y, expectedUvs[corner].y));
        }

        // The second primitive has no normals or UVs.
        // It must not inherit attributes from the first primitive.
        const Mesh& plainMesh = *model.parts[1].mesh;
        assert(plainMesh.triangles.size() == 1);

        const Triangle& plainTriangle = plainMesh.triangles[0];

        for (std::size_t corner = 0; corner < 3; ++corner) {
            assert(!plainTriangle.normals[corner].has_value());
            assert(!plainTriangle.uvs[corner].has_value());
        }
    }
    catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }

    std::filesystem::remove_all(directory);
}