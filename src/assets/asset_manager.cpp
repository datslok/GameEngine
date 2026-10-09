#include "assets/asset_manager.h"
#include "assets/gltf_loader.h"
#include "assets/obj_loader.h"
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

TextureHandle AssetManager::loadTexture(const std::string& path, bool flipVertically) {
    const auto key = std::make_pair(path, flipVertically);

    if (const auto found = texturesByPath.find(key); found != texturesByPath.end()) {
        return found->second;
    }

    const TextureHandle handle = addTexture(loadImage(path, flipVertically));
    texturesByPath.emplace(key, handle);

    return handle;
}

TextureHandle AssetManager::loadTexture(std::shared_ptr<const std::vector<std::uint8_t>> encodedImage, bool flipVertically) {
    if (!encodedImage) {
        throw std::invalid_argument("Cannot load a texture from a null image buffer");
    }

    if (const auto found = texturesByBuffer.find(encodedImage.get()); found != texturesByBuffer.end()) {
        return found->second;
    }

    const TextureHandle handle = addTexture(loadImageFromMemory(
        std::span<const std::uint8_t>{encodedImage->data(), encodedImage->size()},
        flipVertically
    ));

    texturesByBuffer.emplace(encodedImage.get(), handle);
    keptBuffers.push_back(std::move(encodedImage));

    return handle;
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

Material AssetManager::makeMaterial(const MaterialSource& source) {
    if (!source.texturePath.empty() && source.embeddedImage) {
        throw std::invalid_argument("Material must use either a file or an embedded image");
    }

    Material material;
    material.colour = source.colour;

    if (!source.texturePath.empty()) {
        material.texture = loadTexture(source.texturePath, source.flipTextureVertically);
    } else if (source.embeddedImage) {
        material.texture = loadTexture(source.embeddedImage, source.flipTextureVertically);
    }

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
