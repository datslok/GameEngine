#include "gltf_loader.h"

#include "../external/cgltf/cgltf.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <cmath>
#include <cstddef>
#include <utility>

namespace {
    // cgltf data must be released with cgltf_free(), not delete.
    struct GltfDeleter {
        void operator()(cgltf_data* data) const {
            cgltf_free(data);
        }
    };

    void checkGltfResult(
        cgltf_result result,
        const std::string& operation,
        const std::string& filename
    ) {
        if (result != cgltf_result_success) {
            throw std::runtime_error(
                operation + ": " + filename +
                " (cgltf error " +
                std::to_string(static_cast<int>(result)) + ")"
            );
        }
    }
    Mesh readPrimitive(const cgltf_primitive& primitive) {
        if (primitive.type != cgltf_primitive_type_triangles) {
            throw std::runtime_error(
                "Only glTF triangle lists are supported so far"
            );
        }

        if (primitive.targets_count > 0) {
            throw std::runtime_error(
                "glTF morph targets are not supported yet"
            );
        }

        const cgltf_accessor* positions =
            cgltf_find_accessor(
                &primitive,
                cgltf_attribute_type_position,
                0
            );

        if (positions == nullptr ||
            positions->type != cgltf_type_vec3) {
            throw std::runtime_error(
                "glTF primitive needs VEC3 positions"
            );
        }

        if (positions->is_sparse) {
            throw std::runtime_error(
                "Sparse glTF positions are not supported yet"
            );
        }

        Mesh mesh;
        mesh.vertices.reserve(positions->count);

        // Read each position through cgltf so buffer offsets
        // and the spacing between vertices are handled correctly.
        for (std::size_t index = 0;
             index < positions->count;
             ++index) {
            cgltf_float position[3]{};

            if (!cgltf_accessor_read_float(
                    positions, index, position, 3)) {
                throw std::runtime_error(
                    "Could not read glTF vertex position"
                );
            }

            if (!std::isfinite(position[0]) ||
                !std::isfinite(position[1]) ||
                !std::isfinite(position[2])) {
                throw std::runtime_error(
                    "glTF vertex position must be finite"
                );
            }

            mesh.vertices.push_back(Vec4{
                position[0],
                position[1],
                position[2],
                1.0f
            });
        }

        const cgltf_accessor* indices = primitive.indices;

        if (indices != nullptr) {
            const bool unsignedInteger =
                indices->component_type ==
                    cgltf_component_type_r_8u ||
                indices->component_type ==
                    cgltf_component_type_r_16u ||
                indices->component_type ==
                    cgltf_component_type_r_32u;

            if (indices->type != cgltf_type_scalar ||
                !unsignedInteger ||
                indices->normalized ||
                indices->is_sparse) {
                throw std::runtime_error(
                    "Unsupported glTF index format"
                );
            }
        }

        // Without indices, every three consecutive vertices
        // form a triangle.
        const std::size_t cornerCount =
            indices != nullptr
                ? indices->count
                : positions->count;

        if (cornerCount % 3 != 0) {
            throw std::runtime_error(
                "glTF triangle corner count must be divisible by three"
            );
        }

        mesh.triangles.reserve(cornerCount / 3);

        for (std::size_t corner = 0;
             corner < cornerCount;
             corner += 3) {
            std::size_t vertexIndices[3]{};

            for (std::size_t offset = 0; offset < 3; ++offset) {
                const std::size_t vertexIndex =
                    indices != nullptr
                        ? cgltf_accessor_read_index(
                            indices, corner + offset
                        )
                        : corner + offset;

                if (vertexIndex >= mesh.vertices.size()) {
                    throw std::runtime_error(
                        "glTF triangle index is outside the vertex array"
                    );
                }

                vertexIndices[offset] = vertexIndex;
            }

            mesh.triangles.push_back(Triangle{
                vertexIndices[0],
                vertexIndices[1],
                vertexIndices[2]
            });
        }

        return mesh;
    }
    Mat4 readNodeTransform(const cgltf_node& node) {
        cgltf_float values[16]{};
        cgltf_node_transform_world(&node, values);

        Mat4 result;

        // Convert glTF's column-major storage to our [row][column].
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t column = 0; column < 4; ++column) {
                const float value = values[column * 4 + row];

                if (!std::isfinite(value)) {
                    throw std::runtime_error(
                        "glTF node transform must be finite"
                    );
                }

                result.values[row][column] = value;
            }
        }

        return result;
    }

    void appendNode(const cgltf_node& node, Model& model) {
        if (node.skin != nullptr) {
            throw std::runtime_error(
                "Skinned glTF meshes are not supported yet"
            );
        }

        if (node.mesh != nullptr) {
            const Mat4 transform = readNodeTransform(node);

            for (std::size_t index = 0;
                 index < node.mesh->primitives_count;
                 ++index) {
                Mesh mesh = readPrimitive(
                    node.mesh->primitives[index]
                );

                ModelPart part;
                part.mesh =
                    std::make_shared<Mesh>(std::move(mesh));
                part.transform = transform;

                // Keep the default white material for now.
                model.parts.push_back(std::move(part));
            }
        }

        // Visit each child node too.
        for (std::size_t index = 0;
             index < node.children_count;
             ++index) {
            appendNode(*node.children[index], model);
        }
    }
}

Model loadGltf(const std::string& filename) {
    // Default settings for cgltf's memory and file handling.
    const cgltf_options options{};

    cgltf_data* rawData = nullptr;

    // Read the document structure from a .gltf or .glb file.
    const cgltf_result parseResult =
        cgltf_parse_file(&options, filename.c_str(), &rawData);

    // Automatically release the data when this function exits,
    // including when an exception is thrown.
    const std::unique_ptr<cgltf_data, GltfDeleter> data{rawData};

    checkGltfResult(
        parseResult,
        "Could not parse glTF file",
        filename
    );

    // Load binary data containing positions, indices, normals, etc.
    // Passing the filename lets cgltf resolve relative buffer paths.
    checkGltfResult(
        cgltf_load_buffers(&options, data.get(), filename.c_str()),
        "Could not load glTF buffers",
        filename
    );

    // Check the document and loaded buffer data for consistency.
    checkGltfResult(
        cgltf_validate(data.get()),
        "Invalid glTF data",
        filename
    );

    // Reject required extensions until we explicitly support them.
    if (data->extensions_required_count > 0) {
        throw std::runtime_error(
            std::string{"Unsupported required glTF extension: "} +
            data->extensions_required[0]
        );
    }

    // Use the default scene, or the first scene if none is selected.
    const cgltf_scene* scene = data->scene;

    if (scene == nullptr && data->scenes_count > 0) {
        scene = &data->scenes[0];
    }

    if (scene == nullptr) {
        throw std::runtime_error(
            "glTF file contains no scene to load"
        );
    }

    Model model;

    for (std::size_t index = 0;
         index < scene->nodes_count;
         ++index) {
        appendNode(*scene->nodes[index], model);
    }

    return model;
}