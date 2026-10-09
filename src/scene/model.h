#pragma once

#include "core/pixel.h"
#include "math/mat4.h"
#include "scene/mesh.h"
#include "math/vec3.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

/*
* What an imported file says about a part's material: a colour, and where its texture comes from.
* The AssetManager loads the texture once and turns this into a Material with a TextureHandle.
*/
struct MaterialSource {
    Pixel colour{255, 255, 255}; // sRGB

    // A texture can come from a file or embedded image bytes.
    // If both are empty, the part uses plain colour.
    std::string texturePath;

    std::shared_ptr<const std::vector<std::uint8_t>> embeddedImage;

    bool flipTextureVertically = true;

    bool hasTexture() const {
        return !texturePath.empty() || embeddedImage != nullptr;
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

struct ModelBounds {
    Vec3 minimum;
    Vec3 maximum;

    Vec3 centre() const;
    Vec3 size() const;
};

struct Model {
    std::vector<ModelPart> parts;

    ModelBounds getBounds() const;

    // Centre the model and scale its longest side to targetSize.
    Mat4 getNormalizationMatrix(float targetSize = 2.0f) const;
};