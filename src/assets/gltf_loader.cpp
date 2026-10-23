#include "assets/gltf_loader.h"

#include "cgltf/cgltf.h"
#include "core/srgb.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <cmath>
#include <cstddef>
#include <utility>
#include <cstdint>
#include <filesystem>
#include <map>
#include <vector>

namespace {
    // What glTF assumes for a metallic or roughness factor a file leaves out, and for a part with no material.
    constexpr float gltfDefaultFactor = 1.0f;

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
    /*
    * One vertex's tangent: a unit direction (normalised here, as normals are) and a sign that glTF requires to be +1 or
    * -1, the side of the tangent the normal map's "up" is on.
    */
    Vec4 readTangent(const cgltf_accessor& tangents, std::size_t vertexIndex) {
        cgltf_float tangent[4]{};

        if (!cgltf_accessor_read_float(&tangents, vertexIndex, tangent, 4)) {
            throw std::runtime_error("Could not read glTF tangent");
        }

        const Vec3 direction{tangent[0], tangent[1], tangent[2]};
        const float lengthSquared = direction.lengthSquared();

        if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0f || (tangent[3] != 1.0f && tangent[3] != -1.0f)) {
            throw std::runtime_error("glTF tangent must be a finite non-zero direction with w = 1 or -1");
        }

        const Vec3 unit = direction.normalized();
        return Vec4{unit.x, unit.y, unit.z, tangent[3]};
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

        const cgltf_accessor* normals = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);

        const cgltf_accessor* uvs = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0);

        // Optional attributes must have one entry per vertex.
        if (normals != nullptr) {
            if (normals->type != cgltf_type_vec3 ||
                normals->count != positions->count ||
                normals->is_sparse) {
                throw std::runtime_error(
                    "Unsupported glTF normal data"
                );
            }
        }

        if (uvs != nullptr) {
            if (uvs->type != cgltf_type_vec2 ||
                uvs->count != positions->count ||
                uvs->is_sparse) {
                throw std::runtime_error(
                    "Unsupported glTF texture coordinate data"
                );
            }
        }

        const cgltf_accessor* tangents = cgltf_find_accessor(&primitive, cgltf_attribute_type_tangent, 0);

        if (tangents != nullptr) {
            if (tangents->type != cgltf_type_vec4 ||
                tangents->count != positions->count ||
                tangents->is_sparse) {
                throw std::runtime_error(
                    "Unsupported glTF tangent data"
                );
            }
        }

        Mesh mesh;
        // glTF's UV origin is the image's top-left corner.
        mesh.uvOrigin = UvOrigin::TopLeft;
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
            Triangle triangle{};

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

                // The same vertex index selects its position,
                // normal, and texture coordinate.
                if (normals != nullptr) {
                    cgltf_float normal[3]{};

                    if (!cgltf_accessor_read_float(
                            normals, vertexIndex, normal, 3)) {
                        throw std::runtime_error(
                            "Could not read glTF normal"
                        );
                    }

                    if (!std::isfinite(normal[0]) ||
                        !std::isfinite(normal[1]) ||
                        !std::isfinite(normal[2])) {
                        throw std::runtime_error(
                            "glTF normal must be finite"
                        );
                    }

                    const Vec3 direction{
                        normal[0],
                        normal[1],
                        normal[2]
                    };

                    const float lengthSquared =
                        direction.lengthSquared();

                    if (!std::isfinite(lengthSquared) ||
                        lengthSquared <= 0.0f) {
                        throw std::runtime_error(
                            "glTF normal has an invalid length"
                        );
                    }

                    triangle.normals[offset] =
                        direction.normalized();
                }

                if (uvs != nullptr) {
                    cgltf_float uv[2]{};

                    if (!cgltf_accessor_read_float(
                            uvs, vertexIndex, uv, 2)) {
                        throw std::runtime_error(
                            "Could not read glTF texture coordinate"
                        );
                    }

                    if (!std::isfinite(uv[0]) ||
                        !std::isfinite(uv[1])) {
                        throw std::runtime_error(
                            "glTF texture coordinate must be finite"
                        );
                    }

                    // Preserve the file's UV coordinates.
                    // Values outside [0, 1] are valid for texture tiling.
                    triangle.uvs[offset] = Vec2{
                        uv[0],
                        uv[1]
                    };
                }

                if (tangents != nullptr) {
                    triangle.tangents[offset] = readTangent(*tangents, vertexIndex);
                }
            }

            triangle.first = vertexIndices[0];
            triangle.second = vertexIndices[1];
            triangle.third = vertexIndices[2];

            mesh.triangles.push_back(triangle);
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

    // glTF colour factors are linear light, but material colours are sRGB bytes like textures, so encode them.
    std::uint8_t colourChannelToByte(float value) {
        if (!std::isfinite(value) ||
            value < 0.0f ||
            value > 1.0f) {
            throw std::runtime_error(
                "glTF colour channel must be between 0 and 1"
            );
        }

        return linearToSrgbByte(value);
    }

    // glTF's factors (metallic, roughness, emissive, occlusion strength) are shares, 0..1.
    float readUnitFactor(float value, const char* message) {
        if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
            throw std::runtime_error(message);
        }

        return value;
    }

    // A local image file: the URI decoded, checked, and made relative to the model's folder.
    std::string readImagePath(const cgltf_image& image, const std::filesystem::path& modelDirectory) {
        if (image.uri == nullptr || image.uri[0] == '\0') {
            throw std::runtime_error(
                "glTF texture image has no filename"
            );
        }

        std::string uri = image.uri;

        // Reject data URIs and remote URLs for this first version.
        if (uri.find(':') != std::string::npos ||
            uri.starts_with("//") ||
            uri.find_first_of("?#") != std::string::npos) {
            throw std::runtime_error(
                "Only local relative glTF image paths are supported"
            );
        }

        // Convert URI escapes, for example "body%20colour.png".
        const std::size_t decodedLength = cgltf_decode_uri(uri.data());
        uri.resize(decodedLength);

        if (uri.find('\0') != std::string::npos ||
            uri.find(':') != std::string::npos ||
            uri.find('\\') != std::string::npos) {
            throw std::runtime_error(
                "Unsupported glTF image path"
            );
        }

        const std::filesystem::path imagePath{uri};

        if (imagePath.is_absolute() || imagePath.has_root_path()) {
            throw std::runtime_error(
                "glTF image path must be relative"
            );
        }

        return (modelDirectory / imagePath)
            .lexically_normal()
            .generic_string();
    }

    // An image packed into the file's binary buffer (a .glb), copied before cgltf frees its buffers.
    std::shared_ptr<const std::vector<std::uint8_t>> readEmbeddedImage(const cgltf_image& image) {
        if (image.uri != nullptr) {
            throw std::runtime_error(
                "glTF image cannot have both a URI and a buffer view"
            );
        }

        const std::string mimeType =
            image.mime_type != nullptr ? image.mime_type : "";

        if (mimeType != "image/png" && mimeType != "image/jpeg") {
            throw std::runtime_error(
                "Embedded glTF images must be PNG or JPEG"
            );
        }

        const cgltf_buffer_view& bufferView = *image.buffer_view;

        const std::uint8_t* bytes =
            cgltf_buffer_view_data(&bufferView);

        if (bytes == nullptr || bufferView.size == 0) {
            throw std::runtime_error(
                "Embedded glTF image has no data"
            );
        }

        return std::make_shared<const std::vector<std::uint8_t>>(
            bytes,
            bytes + bufferView.size
        );
    }

    /*
    * The file's folder, which image paths are relative to, and each embedded image copied once: several texture slots
    * often name one image (occlusion packed with metallic-roughness, ORM), and handing them all the same bytes lets the
    * AssetManager, which caches embedded images by buffer, decode and upload it once.
    */
    struct GltfImages {
        std::filesystem::path directory;
        std::map<const cgltf_image*, std::shared_ptr<const std::vector<std::uint8_t>>> embedded{};
    };

    /*
    * Where one of a material's textures comes from: nothing for an empty slot, else an external file or embedded bytes.
    * Only the first UV set and textures without a transform are supported.
    */
    TextureSource readTexture(const cgltf_texture_view& view, GltfImages& images) {
        TextureSource texture;

        if (view.texture == nullptr) {
            return texture;
        }

        if (view.texcoord != 0) {
            throw std::runtime_error(
                "Only glTF TEXCOORD_0 is supported so far"
            );
        }

        if (view.has_transform) {
            throw std::runtime_error(
                "glTF texture transforms are not supported yet"
            );
        }

        const cgltf_image* image = view.texture->image;

        if (image == nullptr) {
            throw std::runtime_error(
                "glTF texture has no supported image"
            );
        }

        if (image->buffer_view == nullptr) {
            texture.path = readImagePath(*image, images.directory);
        } else {
            auto [found, added] = images.embedded.try_emplace(image);

            if (added) {
                found->second = readEmbeddedImage(*image);
            }

            texture.embeddedImage = found->second;
        }

        return texture;
    }

    /*
    * glTF's metallic-roughness material. cgltf fills in glTF's defaults for whatever a material leaves out (white,
    * metallic 1, roughness 1, no glow), with or without a pbrMetallicRoughness block; a part with no material gets the
    * same defaults. KHR_materials_emissive_strength lets the glow go past 1.
    */
    MaterialSource readMaterial(const cgltf_material* source, GltfImages& images) {
        MaterialSource material;
        material.flipTextureVertically = false;
        material.metallic = gltfDefaultFactor;
        material.roughness = gltfDefaultFactor;

        if (source == nullptr) {
            return material;
        }

        // Cut-outs (MASK) keep texels whose alpha reaches the cutoff; blended ones (BLEND) cover the share alpha of each
        // pixel, drawn after the opaque scene.
        if (source->alpha_mode == cgltf_alpha_mode_blend) {
            material.alphaMode = AlphaMode::Blend;
        }

        if (source->alpha_mode == cgltf_alpha_mode_mask) {
            if (!std::isfinite(source->alpha_cutoff) || source->alpha_cutoff < 0.0f) {
                throw std::runtime_error(
                    "glTF alpha cutoff must be a number of at least 0"
                );
            }

            material.alphaMode = AlphaMode::Mask;
            material.alphaCutoff = source->alpha_cutoff;
        }

        material.doubleSided = source->double_sided;

        const cgltf_pbr_metallic_roughness& properties = source->pbr_metallic_roughness;

        material.colour = Pixel{
            colourChannelToByte(properties.base_color_factor[0]),
            colourChannelToByte(properties.base_color_factor[1]),
            colourChannelToByte(properties.base_color_factor[2])
        };
        material.opacity = readUnitFactor(properties.base_color_factor[3], "glTF base colour alpha must be between 0 and 1");
        material.metallic = readUnitFactor(properties.metallic_factor, "glTF metallic factor must be between 0 and 1");
        material.roughness = readUnitFactor(properties.roughness_factor, "glTF roughness factor must be between 0 and 1");
        material.colourTexture = readTexture(properties.base_color_texture, images);
        material.metallicRoughnessTexture = readTexture(properties.metallic_roughness_texture, images);
        material.normalTexture = readTexture(source->normal_texture, images);

        if (material.normalTexture.isSet()) {
            if (!std::isfinite(source->normal_texture.scale)) {
                throw std::runtime_error(
                    "glTF normal texture scale must be finite"
                );
            }

            material.normalScale = source->normal_texture.scale;
        }

        // Occlusion in the texture's red channel, whatever else the image packs (ORM).
        material.occlusionTexture = readTexture(source->occlusion_texture, images);

        if (material.occlusionTexture.isSet()) {
            material.occlusionStrength = readUnitFactor(source->occlusion_texture.scale, "glTF occlusion strength must be between 0 and 1");
        }

        material.emissiveTexture = readTexture(source->emissive_texture, images);

        // Thin glass (KHR_materials_transmission): how much light passes through, and a texture (red) multiplying it.
        if (source->has_transmission) {
            material.transmission = readUnitFactor(source->transmission.transmission_factor, "glTF transmission factor must be between 0 and 1");
            material.transmissionTexture = readTexture(source->transmission.transmission_texture, images);
        }

        const float strength = source->has_emissive_strength ? source->emissive_strength.emissive_strength : 1.0f;

        if (!std::isfinite(strength) || strength < 0.0f) {
            throw std::runtime_error(
                "glTF emissive strength must be a positive number"
            );
        }

        const char* const emissiveMessage = "glTF emissive factor must be between 0 and 1";
        material.emissive = Vec3{
            readUnitFactor(source->emissive_factor[0], emissiveMessage),
            readUnitFactor(source->emissive_factor[1], emissiveMessage),
            readUnitFactor(source->emissive_factor[2], emissiveMessage)
        } * strength;

        return material;
    }

    void appendNode(const cgltf_node& node, Model& model, GltfImages& images) {
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
                const cgltf_primitive& primitive =
                    node.mesh->primitives[index];

                Mesh mesh = readPrimitive(primitive);

                ModelPart part;
                part.mesh =
                    std::make_shared<Mesh>(std::move(mesh));

                part.transform = transform;
                part.material = readMaterial(primitive.material, images);

                if (part.material.hasTexture() && cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0) == nullptr) {
                    throw std::runtime_error(
                        "Textured glTF primitive needs TEXCOORD_0"
                    );
                }

                model.parts.push_back(std::move(part));
            }
        }

        // Visit each child node too.
        for (std::size_t index = 0; index < node.children_count; ++index) {
            appendNode(*node.children[index], model, images);
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

    GltfImages images{std::filesystem::path{filename}.parent_path()};

    for (std::size_t index = 0; index < scene->nodes_count; ++index) {
        appendNode(*scene->nodes[index], model, images);
    }

    return model;
}