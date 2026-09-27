#include "gltf_loader.h"
#include "image_loader.h"

#include <cassert>
#include <span>
#include <stdexcept>

void testGlbTextures() {
    const Model model = loadGltf(
        "assets/models/textured_quad.glb"
    );

    assert(model.parts.size() == 1);

    const Material& material = model.parts[0].material;

    assert(material.texturePath.empty());
    assert(material.embeddedImage != nullptr);
    assert(!material.embeddedImage->empty());
    assert(!material.flipTextureVertically);

    // loadGltf() has already freed its cgltf data.
    // The material must still own valid image bytes.
    const auto& bytes = *material.embeddedImage;

    const ImageData embedded = loadImageFromMemory(
        std::span<const std::uint8_t>{
            bytes.data(),
            bytes.size()
        },
        material.flipTextureVertically
    );

    const ImageData external = loadImage(
        "assets/textures/demo.png",
        false
    );

    assert(embedded.width == external.width);
    assert(embedded.height == external.height);
    assert(embedded.pixels == external.pixels);

    // Invalid input must produce an exception.
    bool rejectedEmpty = false;

    try {
        loadImageFromMemory(
            std::span<const std::uint8_t>{},
            false
        );
    } catch (const std::invalid_argument&) {
        rejectedEmpty = true;
    }

    assert(rejectedEmpty);
}