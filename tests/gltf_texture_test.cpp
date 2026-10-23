#include "assets/asset_manager.h"
#include "assets/gltf_loader.h"
#include "math/transform.h"
#include "scene/model_renderer.h"

#include <cassert>
#include <cmath>
#include <filesystem>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }

    /*
    * The water bottle (Khronos's sample, CC0) packs occlusion, roughness and metallic in one image (ORM) that two of its
    * texture slots name, and glows through an emissive texture at full factor. Both slots get the same bytes, so the
    * image is decoded and uploaded once.
    */
    void testWaterBottleSharesItsPackedImage() {
        const Model model = loadGltf("assets/models/WaterBottle.glb");
        assert(model.parts.size() == 1);

        const MaterialSource& material = model.parts[0].material;
        assert(material.occlusionTexture.embeddedImage != nullptr);
        assert(material.occlusionTexture.embeddedImage == material.metallicRoughnessTexture.embeddedImage);
        assert(nearlyEqual(material.occlusionStrength, 1.0f));
        assert(material.emissiveTexture.embeddedImage != nullptr);
        assert(material.emissiveTexture.embeddedImage != material.colourTexture.embeddedImage);
        assert(nearlyEqual(material.emissive.x, 1.0f) && nearlyEqual(material.emissive.y, 1.0f) && nearlyEqual(material.emissive.z, 1.0f));

        AssetManager assets;
        const ModelRenderer renderer = assets.makeModelRenderer(model);
        const Material& made = renderer.parts[0].material;
        assert(made.occlusionTexture == made.metallicRoughnessTexture);
        assert(assets.getTextureCount() == 4);
    }
}

void testGltfTextures() {
    // An absolute filename checks that textures resolve relative
    // to the model's directory.
    const std::filesystem::path filename =
        std::filesystem::absolute(
            "assets/models/textured_quad.gltf"
        ).lexically_normal();

    const Model model = loadGltf(filename.string());

    assert(model.parts.size() == 1);

    const ModelPart& part = model.parts[0];
    assert(part.mesh != nullptr);
    assert(part.mesh->vertices.size() == 4);
    assert(part.mesh->triangles.size() == 2);

    // Check both URI decoding and relative-path resolution.
    const std::string expectedTexture =
        (filename.parent_path() / "../textures/demo.png")
            .lexically_normal()
            .generic_string();

    assert(part.material.colourTexture.path == expectedTexture);
    assert(!part.material.flipTextureVertically);
    assert(std::filesystem::is_regular_file(expectedTexture));

    assert(part.material.colour.r == 255);
    assert(part.material.colour.g == 255);
    assert(part.material.colour.b == 255);

    const Triangle& triangle = part.mesh->triangles[0];

    assert(triangle.first == 0);
    assert(triangle.second == 1);
    assert(triangle.third == 2);

    assert(triangle.uvs[0].has_value());
    assert(nearlyEqual(triangle.uvs[0]->x, 0.0f));
    assert(nearlyEqual(triangle.uvs[0]->y, 1.0f));

    // The imported node translates its mesh upwards by 0.5.
    const Vec4 importedOrigin =
        part.transform * Vec4{0.0f, 0.0f, 0.0f, 1.0f};

    assert(nearlyEqual(importedOrigin.y, 0.5f));

    // Verify that placing the model in the world also preserves that transform.
    AssetManager assets;
    const ModelRenderer renderer = assets.makeModelRenderer(model);

    // The external texture is loaded once and referred to by handle, unflipped as glTF requires.
    assert(renderer.parts[0].material.texture.isValid());
    assert(assets.getTexture(renderer.parts[0].material.texture).pixels ==
           assets.getTexture(assets.loadTexture(expectedTexture, false)).pixels);
    assert(assets.getTextureCount() == 1);

    Transform placement;
    placement.position = Vec3{0.0f, 0.0f, -6.0f};
    placement.scale = Vec3{2.0f, 2.0f, 2.0f};

    const Vec4 worldOrigin =
        placement.getMatrix() * renderer.parts[0].localTransform *
        Vec4{0.0f, 0.0f, 0.0f, 1.0f};

    assert(nearlyEqual(worldOrigin.x, 0.0f));
    assert(nearlyEqual(worldOrigin.y, 1.0f));
    assert(nearlyEqual(worldOrigin.z, -6.0f));

    testWaterBottleSharesItsPackedImage();
}