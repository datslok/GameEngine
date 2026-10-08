#include "ecs/world.h"
#include "math/transform.h"
#include "scene/model_renderer.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
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
}

void testModelRenderer() {
    auto mesh = std::make_shared<Mesh>();

    ModelPart first;
    first.mesh = mesh;
    first.transform = Mat4::translation(2.0f, 0.0f, 0.0f);
    first.material.colour = Pixel{10, 20, 30};
    first.material.texturePath = "first.png";
    first.material.flipTextureVertically = false;

    ModelPart second;
    second.mesh = mesh;
    second.transform = Mat4::translation(-2.0f, 0.0f, 0.0f);
    second.material.colour = Pixel{40, 50, 60};
    second.material.embeddedImage =
        std::make_shared<const std::vector<std::uint8_t>>(
            std::initializer_list<std::uint8_t>{1, 2, 3}
        );

    Model model;
    model.parts = {first, second};

    // Parts keep their mesh and material, and the normalization sits below each part's own transform.
    const ModelRenderer renderer = makeModelRenderer(model, Mat4::scaling(0.5f, 0.5f, 0.5f));

    assert(renderer.parts.size() == 2);
    assert(renderer.visible);

    const RenderPart& a = renderer.parts[0];
    const RenderPart& b = renderer.parts[1];

    assert(a.mesh == mesh);
    assert(b.mesh == mesh);
    assert(a.material.colour.r == 10);
    assert(a.material.colour.g == 20);
    assert(a.material.colour.b == 30);
    assert(a.material.texturePath == "first.png");
    assert(!a.material.flipTextureVertically);
    assert(b.material.colour.r == 40);
    assert(b.material.embeddedImage == second.material.embeddedImage);

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

    // Two entities can show the same model with independent transforms.
    World world;
    const Entity left = world.create();
    const Entity right = world.create();
    world.add(left, makeModelRenderer(model));
    world.add(right, makeModelRenderer(model));
    world.add(left, Transform{});
    world.add(right, Transform{});

    world.get<Transform>(right).position.x = 10.0f;

    assertPartOrigin(world.get<Transform>(left), world.get<ModelRenderer>(left).parts[0], 2.0f, 0.0f, 0.0f);
    assertPartOrigin(world.get<Transform>(right), world.get<ModelRenderer>(right).parts[0], 12.0f, 0.0f, 0.0f);
    assert(world.get<ModelRenderer>(left).parts[0].mesh == world.get<ModelRenderer>(right).parts[0].mesh);

    // A bad part is rejected before anything is built.
    Model invalid;
    invalid.parts.push_back(first);
    invalid.parts.push_back(ModelPart{});

    bool rejected = false;
    try {
        makeModelRenderer(invalid);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);

    // An empty model gives a renderer that draws nothing.
    assert(makeModelRenderer(Model{}).parts.empty());

    // A single mesh becomes one part at the entity's origin.
    Material red;
    red.colour = Pixel{255, 0, 0};
    const ModelRenderer single = makeMeshRenderer(mesh, red);
    assert(single.parts.size() == 1);
    assert(single.parts[0].mesh == mesh);
    assert(single.parts[0].material.colour.r == 255);
    assertPartOrigin(Transform{}, single.parts[0], 0.0f, 0.0f, 0.0f);

    rejected = false;
    try {
        makeMeshRenderer(nullptr);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
