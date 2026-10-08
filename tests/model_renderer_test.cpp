#include "assets/asset_manager.h"
#include "ecs/world.h"
#include "math/transform.h"
#include "scene/model_renderer.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <vector>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }

    // Where a part's origin ends up in the world.
    void assertPartOrigin(const Transform& transform, const RenderPart& part, float x, float y, float z) {
        const Vec4 origin = transform.getMatrix() * part.localTransform * Vec4{0.0f, 0.0f, 0.0f, 1.0f};

        assert(nearlyEqual(origin.x, x));
        assert(nearlyEqual(origin.y, y));
        assert(nearlyEqual(origin.z, z));
    }

    template <typename Function>
    bool throws(Function function) {
        try {
            function();
        } catch (const std::exception&) {
            return true;
        }
        return false;
    }
}

void testModelRenderer() {
    auto mesh = std::make_shared<const Mesh>(Mesh::cube());

    std::ifstream file{"assets/textures/demo.png", std::ios::binary};
    assert(file);
    const auto imageBytes = std::make_shared<const std::vector<std::uint8_t>>(
        std::istreambuf_iterator<char>{file},
        std::istreambuf_iterator<char>{}
    );

    ModelPart first;
    first.mesh = mesh;
    first.transform = Mat4::translation(2.0f, 0.0f, 0.0f);
    first.material.colour = Pixel{10, 20, 30};
    first.material.texturePath = "assets/textures/demo.png";
    first.material.flipTextureVertically = false;

    ModelPart second;
    second.mesh = mesh;
    second.transform = Mat4::translation(-2.0f, 0.0f, 0.0f);
    second.material.colour = Pixel{40, 50, 60};
    second.material.embeddedImage = imageBytes;
    second.material.flipTextureVertically = false;

    Model model;
    model.parts = {first, second};

    AssetManager assets;

    // Parts become handles, and the normalization sits below each part's own transform.
    const ModelRenderer renderer = assets.makeModelRenderer(model, Mat4::scaling(0.5f, 0.5f, 0.5f));

    assert(renderer.parts.size() == 2);
    assert(renderer.visible);

    const RenderPart& a = renderer.parts[0];
    const RenderPart& b = renderer.parts[1];

    // Both parts share one mesh, so they share one handle.
    assert(a.mesh.isValid());
    assert(a.mesh == b.mesh);
    assert(assets.getMeshCount() == 1);

    assert(a.material.colour.r == 10);
    assert(a.material.colour.g == 20);
    assert(a.material.colour.b == 30);
    assert(b.material.colour.r == 40);

    // A file texture and an embedded copy of the same image are different sources, so two textures with the same pixels.
    assert(a.material.texture.isValid());
    assert(b.material.texture.isValid());
    assert(!(a.material.texture == b.material.texture));
    assert(assets.getTexture(a.material.texture).pixels == assets.getTexture(b.material.texture).pixels);

    // One entity Transform moves, rotates and scales every part together.
    Transform placement;
    placement.position = Vec3{0.0f, 0.0f, -6.0f};
    placement.rotation.z = std::numbers::pi_v<float> / 2.0f;

    assertPartOrigin(placement, a, 0.0f, 1.0f, -6.0f);
    assertPartOrigin(placement, b, 0.0f, -1.0f, -6.0f);

    placement.position.x = 3.0f;
    placement.scale = Vec3{2.0f, 2.0f, 2.0f};

    assertPartOrigin(placement, a, 3.0f, 2.0f, -6.0f);
    assertPartOrigin(placement, b, 3.0f, -2.0f, -6.0f);

    // Two entities can show the same model with independent transforms and no extra assets.
    World world;
    const Entity left = world.create();
    const Entity right = world.create();
    world.add(left, assets.makeModelRenderer(model));
    world.add(right, assets.makeModelRenderer(model));
    world.add(left, Transform{});
    world.add(right, Transform{});

    world.get<Transform>(right).position.x = 10.0f;

    assertPartOrigin(world.get<Transform>(left), world.get<ModelRenderer>(left).parts[0], 2.0f, 0.0f, 0.0f);
    assertPartOrigin(world.get<Transform>(right), world.get<ModelRenderer>(right).parts[0], 12.0f, 0.0f, 0.0f);
    assert(world.get<ModelRenderer>(left).parts[0].mesh == world.get<ModelRenderer>(right).parts[0].mesh);
    assert(assets.getMeshCount() == 1);
    assert(assets.getTextureCount() == 2);

    // A part without a mesh is rejected before anything is registered.
    Model invalid;
    invalid.parts.push_back(first);
    invalid.parts.push_back(ModelPart{});

    AssetManager fresh;
    assert(throws([&] { fresh.makeModelRenderer(invalid); }));
    assert(fresh.getMeshCount() == 0);

    // A material cannot name both a file and embedded bytes.
    MaterialSource both;
    both.texturePath = "assets/textures/demo.png";
    both.embeddedImage = imageBytes;
    assert(throws([&] { fresh.makeMaterial(both); }));

    // A material without a texture keeps "no texture", which the renderer draws as plain colour.
    MaterialSource plain;
    plain.colour = Pixel{1, 2, 3};
    const Material plainMaterial = fresh.makeMaterial(plain);
    assert(!plainMaterial.texture.isValid());
    assert(plainMaterial.colour.b == 3);

    // An empty model gives a renderer that draws nothing.
    assert(fresh.makeModelRenderer(Model{}).parts.empty());

    // A single mesh becomes one part at the entity's origin.
    Material red;
    red.colour = Pixel{255, 0, 0};
    const MeshHandle cube = fresh.addMesh(Mesh::cube());
    const ModelRenderer single = makeMeshRenderer(cube, red);
    assert(single.parts.size() == 1);
    assert(single.parts[0].mesh == cube);
    assert(single.parts[0].material.colour.r == 255);
    assertPartOrigin(Transform{}, single.parts[0], 0.0f, 0.0f, 0.0f);

    assert(throws([&] { makeMeshRenderer(MeshHandle{}); }));
}
