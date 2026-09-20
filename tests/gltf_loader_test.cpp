#include "gltf_loader.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <stdexcept>

void testGltfLoader() {
    // Create a unique temporary directory for this test.
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        "game_engine_gltf_loader_test";

    // Refuse to overwrite files from an existing directory.
    if (!std::filesystem::create_directory(directory)) {
        throw std::runtime_error(
            "glTF test directory already exists: " +
            directory.string()
        );
    }

    try {
        // A missing file must throw.
        bool missingFileThrew = false;

        try {
            loadGltf((directory / "missing.gltf").string());
        }
        catch (const std::runtime_error&) {
            missingFileThrew = true;
        }

        assert(missingFileThrew);

        // A file containing invalid data must also throw.
        const std::filesystem::path invalidFile =
            directory / "invalid.gltf";

        {
            std::ofstream output{invalidFile};

            if (!output) {
                throw std::runtime_error(
                    "Could not create glTF test file"
                );
            }

            output << "This is not a glTF document.";
        }

        bool invalidFileThrew = false;

        try {
            loadGltf(invalidFile.string());
        }
        catch (const std::runtime_error&) {
            invalidFileThrew = true;
        }

        assert(invalidFileThrew);
        // Three positions followed by three unsigned 16-bit indices.
        // glTF binary values use little-endian byte order.
        const unsigned char triangleBytes[] = {
            // Vertex 0: (0, 0, 0)
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,

            // Vertex 1: (1, 0, 0)
            0x00, 0x00, 0x80, 0x3F,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,

            // Vertex 2: (0, 1, 0)
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x80, 0x3F,
            0x00, 0x00, 0x00, 0x00,

            // Triangle indices: 2, 0, 1
            0x02, 0x00,
            0x00, 0x00,
            0x01, 0x00
        };

        {
            std::ofstream output{
                directory / "triangle.bin",
                std::ios::binary
            };

            output.write(
                reinterpret_cast<const char*>(triangleBytes),
                sizeof(triangleBytes)
            );

            output.close();

            if (!output) {
                throw std::runtime_error(
                    "Could not write triangle test buffer"
                );
            }
        }

        const std::filesystem::path triangleFile =
            directory / "triangle.gltf";

        {
            std::ofstream output{triangleFile};

            output << R"json(
{
    "asset": {
        "version": "2.0"
    },
    "scene": 0,
    "scenes": [
        { "nodes": [0] }
    ],
    "nodes": [
        {
            "translation": [10, 20, 30],
            "scale": [2, 3, 4],
            "children": [1]
        },
        {
            "translation": [1, 2, 3],
            "mesh": 0
        }
    ],
    "meshes": [
        {
            "primitives": [
                {
                    "attributes": { "POSITION": 0 },
                    "indices": 1,
                    "mode": 4
                }
            ]
        }
    ],
    "buffers": [
        {
            "uri": "triangle.bin",
            "byteLength": 42
        }
    ],
    "bufferViews": [
        {
            "buffer": 0,
            "byteOffset": 0,
            "byteLength": 36,
            "target": 34962
        },
        {
            "buffer": 0,
            "byteOffset": 36,
            "byteLength": 6,
            "target": 34963
        }
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
                    "Could not write triangle test document"
                );
            }
        }

        const Model loadedModel = loadGltf(triangleFile.string());

        assert(loadedModel.parts.size() == 1);

        const ModelPart& loadedPart = loadedModel.parts[0];
        assert(loadedPart.mesh != nullptr);

        const Mesh& loadedMesh = *loadedPart.mesh;

        assert(loadedMesh.vertices.size() == 3);
        assert(loadedMesh.triangles.size() == 1);

        // Positions remain in the mesh's local coordinates.
        const float expectedPositions[3][3] = {
            {0.0f, 0.0f, 0.0f},
            {1.0f, 0.0f, 0.0f},
            {0.0f, 1.0f, 0.0f}
        };

        for (std::size_t index = 0; index < 3; ++index) {
            const Vec4& vertex = loadedMesh.vertices[index];

            assert(vertex.x == expectedPositions[index][0]);
            assert(vertex.y == expectedPositions[index][1]);
            assert(vertex.z == expectedPositions[index][2]);
            assert(vertex.w == 1.0f);
        }

        // Check that the loader used the index buffer.
        const Triangle& loadedTriangle = loadedMesh.triangles[0];

        assert(loadedTriangle.first == 2);
        assert(loadedTriangle.second == 0);
        assert(loadedTriangle.third == 1);

        // Parent scale also scales the child's translation:
        // (10, 20, 30) + (2, 3, 4) * (1, 2, 3)
        // = (12, 26, 42).
        const float expectedTransform[4][4] = {
            {2.0f, 0.0f, 0.0f, 12.0f},
            {0.0f, 3.0f, 0.0f, 26.0f},
            {0.0f, 0.0f, 4.0f, 42.0f},
            {0.0f, 0.0f, 0.0f,  1.0f}
        };

        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                assert(
                    loadedPart.transform.values[row][column] ==
                    expectedTransform[row][column]
                );
            }
        }
    }
    catch (...) {
        std::filesystem::remove_all(directory);
        throw;
    }

    std::filesystem::remove_all(directory);
}