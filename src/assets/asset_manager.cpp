#include "assets/asset_manager.h"
#include "assets/gltf_loader.h"
#include "assets/obj_loader.h"
#include "scene/collision_mesh.h"
#include "scene/smooth_normals.h"

#include <span>
#include <stdexcept>

MeshHandle AssetManager::addMesh(Mesh mesh) {
    return addMesh(std::make_shared<const Mesh>(std::move(mesh)));
}

/*
* Imported models share their meshes through pointers. Remembering each pointer means two ducks from one model share one handle and one GPU upload.
*/
MeshHandle AssetManager::addMesh(std::shared_ptr<const Mesh> mesh) {
    if (!mesh) {
        throw std::invalid_argument("Cannot add a null mesh");
    }

    if (const auto found = meshesByPointer.find(mesh.get()); found != meshesByPointer.end()) {
        return found->second;
    }

    const MeshHandle handle{static_cast<std::uint32_t>(meshes.size())};
    meshesByPointer.emplace(mesh.get(), handle);
    meshes.push_back(std::move(mesh));

    return handle;
}

/*
* The options are part of the cache key, so the same file loaded flat and smooth gives two meshes.
*/
MeshHandle AssetManager::loadMesh(const std::string& path, MeshLoadOptions options) {
    const auto key = std::make_tuple(path, options.smoothNormals, options.smoothNormals ? options.creaseAngle : 0.0f);

    if (const auto found = meshesByPath.find(key); found != meshesByPath.end()) {
        return found->second;
    }

    // Load before recording the path, so a failed load is not remembered.
    Mesh mesh = loadObj(path);

    if (options.smoothNormals) {
        generateSmoothNormals(mesh, options.creaseAngle);
    }

    const MeshHandle handle = addMesh(std::move(mesh));
    meshesByPath.emplace(key, handle);

    return handle;
}

TextureHandle AssetManager::loadTexture(const std::string& path, bool flipVertically, TextureEncoding encoding) {
    const auto key = std::make_tuple(path, flipVertically, encoding);

    if (const auto found = texturesByPath.find(key); found != texturesByPath.end()) {
        return found->second;
    }

    ImageData image = loadImage(path, flipVertically);
    image.encoding = encoding;

    const TextureHandle handle = addTexture(std::move(image));
    texturesByPath.emplace(key, handle);

    return handle;
}

TextureHandle AssetManager::loadTexture(std::shared_ptr<const std::vector<std::uint8_t>> encodedImage, bool flipVertically, TextureEncoding encoding) {
    if (!encodedImage) {
        throw std::invalid_argument("Cannot load a texture from a null image buffer");
    }

    const auto key = std::make_pair(encodedImage.get(), encoding);

    if (const auto found = texturesByBuffer.find(key); found != texturesByBuffer.end()) {
        return found->second;
    }

    ImageData image = loadImageFromMemory(std::span<const std::uint8_t>{encodedImage->data(), encodedImage->size()}, flipVertically);
    image.encoding = encoding;

    const TextureHandle handle = addTexture(std::move(image));
    texturesByBuffer.emplace(key, handle);
    keptBuffers.push_back(std::move(encodedImage));

    return handle;
}

TextureHandle AssetManager::loadTexture(const TextureSource& source, bool flipVertically, TextureEncoding encoding) {
    if (!source.path.empty() && source.embeddedImage) {
        throw std::invalid_argument("A texture must come from either a file or an embedded image");
    }

    if (!source.path.empty()) {
        return loadTexture(source.path, flipVertically, encoding);
    }

    if (source.embeddedImage) {
        return loadTexture(source.embeddedImage, flipVertically, encoding);
    }

    return TextureHandle{};
}

TextureHandle AssetManager::addTexture(ImageData image) {
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != static_cast<std::size_t>(image.width) * image.height * 4) {
        throw std::invalid_argument("Texture needs four bytes for each of its pixels");
    }

    const TextureHandle handle{static_cast<std::uint32_t>(textures.size())};
    textures.push_back(std::move(image));

    return handle;
}

const Model& AssetManager::loadModel(const std::string& path) {
    if (const auto found = modelsByPath.find(path); found != modelsByPath.end()) {
        return found->second;
    }

    return modelsByPath.emplace(path, loadGltf(path)).first->second;
}

/*
* The colour texture holds colours (sRGB), the metallic-roughness texture numbers and the normal map directions (both
* linear). All are read with the same UVs, so they share the flip.
*/
Material AssetManager::makeMaterial(const MaterialSource& source) {
    Material material;
    material.colour = source.colour;
    material.metallic = source.metallic;
    material.roughness = source.roughness;
    material.emissive = source.emissive;
    material.texture = loadTexture(source.colourTexture, source.flipTextureVertically, TextureEncoding::Srgb);
    material.metallicRoughnessTexture = loadTexture(source.metallicRoughnessTexture, source.flipTextureVertically, TextureEncoding::Linear);
    material.normalTexture = loadTexture(source.normalTexture, source.flipTextureVertically, TextureEncoding::Linear);
    material.normalScale = source.normalScale;

    return material;
}

ModelRenderer AssetManager::makeModelRenderer(const Model& model, const Mat4& normalization) {
    for (const ModelPart& part : model.parts) {
        if (!part.mesh) {
            throw std::invalid_argument("Cannot render a model part with a null mesh");
        }
    }

    ModelRenderer renderer;
    renderer.parts.reserve(model.parts.size());

    for (const ModelPart& part : model.parts) {
        // Folding the normalization in here means it is multiplied once, not for every part on every frame.
        renderer.parts.push_back(RenderPart{
            addMesh(part.mesh),
            makeMaterial(part.material),
            normalization * part.transform
        });
    }

    return renderer;
}

const Mesh& AssetManager::getMesh(MeshHandle handle) const {
    if (!handle.isValid() || handle.index >= meshes.size()) {
        throw std::out_of_range("Unknown mesh handle");
    }

    return *meshes[handle.index];
}

const ImageData& AssetManager::getTexture(TextureHandle handle) const {
    if (!handle.isValid() || handle.index >= textures.size()) {
        throw std::out_of_range("Unknown texture handle");
    }

    return textures[handle.index];
}

std::size_t AssetManager::getMeshCount() const {
    return meshes.size();
}

std::size_t AssetManager::getTextureCount() const {
    return textures.size();
}

/*
* Each part's triangles go in with the part's local transform (normalization and imported node transforms included), so
* the mesh lines up with what the renderer draws for the same entity.
*/
TriangleMesh AssetManager::makeCollisionMesh(const ModelRenderer& renderer) const {
    TriangleMesh result;
    for (const RenderPart& part : renderer.parts) {
        appendCollisionMesh(result, getMesh(part.mesh), part.localTransform);
    }
    return result;
}
