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

    assert(part.material.texturePath == expectedTexture);
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
    const ModelRenderer renderer = makeModelRenderer(model);

    Transform placement;
    placement.position = Vec3{0.0f, 0.0f, -6.0f};
    placement.scale = Vec3{2.0f, 2.0f, 2.0f};

    const Vec4 worldOrigin =
        placement.getMatrix() * renderer.parts[0].localTransform *
        Vec4{0.0f, 0.0f, 0.0f, 1.0f};

    assert(nearlyEqual(worldOrigin.x, 0.0f));
    assert(nearlyEqual(worldOrigin.y, 1.0f));
    assert(nearlyEqual(worldOrigin.z, -6.0f));
}