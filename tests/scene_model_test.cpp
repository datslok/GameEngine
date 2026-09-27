#include "scene.h"

#include <cassert>
#include <cmath>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <initializer_list>

namespace {
    bool nearlyEqual(float actual, float expected) {
        return std::abs(actual - expected) < 0.00001f;
    }
}

void testSceneModel() {
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

    Scene scene;

    // Adding a model must preserve existing scene objects.
    scene.add(MeshInstance{mesh});

    Transform placement;
    placement.position = Vec3{0.0f, 0.0f, -6.0f};
    placement.rotation.z = std::numbers::pi_v<float> / 2.0f;

    scene.addModel(
        model,
        placement,
        Mat4::scaling(0.5f, 0.5f, 0.5f),
        Vec3{0.0f, 0.5f, 0.0f}
    );

    const auto& objects = scene.getObjects();

    assert(objects.size() == 3);

    const MeshInstance& a = objects[1];
    const MeshInstance& b = objects[2];

    // Meshes and embedded image bytes remain shared.
    assert(a.mesh == mesh);
    assert(b.mesh == mesh);
    assert(b.material.embeddedImage == second.material.embeddedImage);

    // Materials remain specific to their model parts.
    assert(a.material.colour.r == 10);
    assert(a.material.colour.g == 20);
    assert(a.material.colour.b == 30);
    assert(a.material.texturePath == "first.png");
    assert(!a.material.flipTextureVertically);
    assert(b.material.colour.r == 40);
    assert(b.material.texturePath.empty());

    // Initial rotation must survive the application's update loop.
    assert(nearlyEqual(
        a.initialRotation.z,
        placement.rotation.z
    ));
    assert(nearlyEqual(a.rotationSpeed.y, 0.5f));
    assert(nearlyEqual(b.rotationSpeed.y, 0.5f));

    // Imported translation -> normalization -> scene placement.
    // The two part origins become (0, 1, -6) and (0, -1, -6).
    const Vec4 origin{0.0f, 0.0f, 0.0f, 1.0f};

    const Vec4 firstPosition = a.getModelMatrix() * origin;
    const Vec4 secondPosition = b.getModelMatrix() * origin;

    assert(nearlyEqual(firstPosition.x, 0.0f));
    assert(nearlyEqual(firstPosition.y, 1.0f));
    assert(nearlyEqual(firstPosition.z, -6.0f));

    assert(nearlyEqual(secondPosition.x, 0.0f));
    assert(nearlyEqual(secondPosition.y, -1.0f));
    assert(nearlyEqual(secondPosition.z, -6.0f));

    // A bad later part must not leave a partially added model.
    Model invalid;
    invalid.parts.push_back(first);
    invalid.parts.push_back(ModelPart{});

    bool rejected = false;

    try {
        scene.addModel(invalid);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }

    assert(rejected);
    assert(scene.getObjects().size() == 3);

    // An empty model adds nothing.
    scene.addModel(Model{});
    assert(scene.getObjects().size() == 3);
}