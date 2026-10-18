#pragma once

#include "assets/image_loader.h"
#include "math/angles.h"
#include "math/mat4.h"
#include "physics/triangle_mesh.h"
#include "scene/asset_handles.h"
#include "scene/material.h"
#include "scene/mesh.h"
#include "scene/model.h"
#include "scene/model_renderer.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// How to prepare a mesh file. The defaults leave the file exactly as it is.
struct MeshLoadOptions {
    // Fill in smooth normals for corners the file gives none. Off by default: only the game knows whether a shape is meant to be faceted.
    bool smoothNormals = false;

    // Faces meeting at more than this angle (radians) keep a sharp edge when smoothing.
    float creaseAngle = radiansFromDegrees(60.0f);
};

/*
* Owns every loaded mesh and texture on the CPU side and hands out handles to them.
* Each file is loaded once: asking for the same path again returns the same handle.
* Assets are only ever appended, so "which assets are new since last frame" is every handle from the last count onwards.
*/
class AssetManager {
public:
    // Register a mesh made in code, such as a cube or a ground plane.
    MeshHandle addMesh(Mesh mesh);

    // Register a mesh that is already shared, such as one from an imported model. The same pointer always gives the same handle.
    MeshHandle addMesh(std::shared_ptr<const Mesh> mesh);

    // Load an OBJ file, or return the handle from the first time it was loaded with the same options.
    MeshHandle loadMesh(const std::string& path, MeshLoadOptions options = {});

    // Load an image file, or return the handle from the first time it was loaded with the same flip setting and encoding.
    // Rows are flipped by default to match bottom-left UV coordinates (OBJ); glTF textures use flipVertically = false.
    // Colour images are sRGB; data images (metallic-roughness) are TextureEncoding::Linear.
    TextureHandle loadTexture(const std::string& path, bool flipVertically = true, TextureEncoding encoding = TextureEncoding::Srgb);

    // Decode an image held in memory (embedded in a .glb). The same byte buffer and encoding always give the same handle.
    TextureHandle loadTexture(std::shared_ptr<const std::vector<std::uint8_t>> encodedImage, bool flipVertically, TextureEncoding encoding = TextureEncoding::Srgb);

    // Load whichever a source names, a file or embedded bytes; "none" for a source that names neither. Throws
    // std::invalid_argument for one that names both.
    TextureHandle loadTexture(const TextureSource& source, bool flipVertically, TextureEncoding encoding);

    // Register an image made in code.
    TextureHandle addTexture(ImageData image);

    // Load a glTF/GLB model, or return the one loaded the first time. The reference stays valid for the manager's lifetime.
    const Model& loadModel(const std::string& path);

    // Load the textures a file describes (if any) and give back a Material that refers to them by handle, with its
    // metallic, roughness and glow as the file gives them.
    Material makeMaterial(const MaterialSource& source);

    // Turn an imported model into a renderer component: meshes and textures become handles, and normalization is folded into each part.
    // A part without a mesh is rejected before anything is registered. A texture that fails to load can still throw part-way; the parts already registered stay cached, which is harmless.
    ModelRenderer makeModelRenderer(const Model& model, const Mat4& normalization = Mat4::identity());

    // One collision mesh with every part of a renderer: each part's triangles placed by its local transform, in the
    // entity's own coordinates (like a Collider). Throws std::out_of_range for parts whose mesh this manager does not have.
    TriangleMesh makeCollisionMesh(const ModelRenderer& renderer) const;

    // Throw std::out_of_range for handles this manager did not create, including "none".
    const Mesh& getMesh(MeshHandle handle) const;
    const ImageData& getTexture(TextureHandle handle) const;

    std::size_t getMeshCount() const;
    std::size_t getTextureCount() const;

private:
    // Shared pointers keep each mesh at a fixed address, so references returned by getMesh stay valid as more meshes are added.
    std::vector<std::shared_ptr<const Mesh>> meshes;
    std::vector<ImageData> textures;

    // std::map never moves its elements, so references returned by loadModel stay valid.
    std::map<std::string, Model> modelsByPath;

    // Keyed by path, whether smoothed, and crease angle (0 when not smoothed, since it is unused then).
    std::map<std::tuple<std::string, bool, float>, MeshHandle> meshesByPath;
    std::map<const Mesh*, MeshHandle> meshesByPointer;
    std::map<std::tuple<std::string, bool, TextureEncoding>, TextureHandle> texturesByPath;

    // Keyed by the buffer's address. The buffers are kept alive here, so an address cannot be reused by a different image.
    std::map<std::pair<const std::vector<std::uint8_t>*, TextureEncoding>, TextureHandle> texturesByBuffer;
    std::vector<std::shared_ptr<const std::vector<std::uint8_t>>> keptBuffers;
};
