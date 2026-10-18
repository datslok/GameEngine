#pragma once

#include "core/pixel.h"
#include "math/aabb.h"
#include "math/mat4.h"
#include "scene/material.h"
#include "scene/mesh.h"
#include "math/vec3.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/*
* Where one of a material's textures comes from: an image file, or image bytes embedded in the model file (a .glb).
* Neither means no texture.
*/
struct TextureSource {
    std::string path;
    std::shared_ptr<const std::vector<std::uint8_t>> embeddedImage;

    bool isSet() const {
        return !path.empty() || embeddedImage != nullptr;
    }
};

/*
* What an imported file says about a part's material: colour, metallic, roughness, glow, and where its textures come from.
* The AssetManager loads the textures once and turns this into a Material with TextureHandles.
*/
struct MaterialSource {
    Pixel colour{255, 255, 255}; // sRGB
    TextureSource colourTexture;

    // Multiplied by the texture's green (roughness) and blue (metallic) channels when there is one. The defaults are
    // Material's own.
    float metallic = Material{}.metallic;
    float roughness = Material{}.roughness;
    TextureSource metallicRoughnessTexture;

    Vec3 emissive{0.0f, 0.0f, 0.0f}; // linear light

    // For every texture of the material: they share one set of UVs.
    bool flipTextureVertically = true;

    bool hasTexture() const {
        return colourTexture.isSet() || metallicRoughnessTexture.isSet();
    }
};

// One piece of an imported model, as loaded from the file.
struct ModelPart {
    std::shared_ptr<const Mesh> mesh;
    MaterialSource material;

    // Positions this part within the model.
    // Includes any parent transforms from the imported file.
    Mat4 transform = Mat4::identity();
};

// The smallest axis-aligned box around a mesh's vertices, in its own coordinates. Empty meshes give a zero box at the origin.
Aabb meshBounds(const Mesh& mesh);

struct Model {
    std::vector<ModelPart> parts;

    Aabb getBounds() const;

    // Centre the model and scale its longest side to targetSize.
    Mat4 getNormalizationMatrix(float targetSize = 2.0f) const;
};