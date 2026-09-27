#include "scene.h"

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

    void assertPosition(
        const MeshInstance& object,
        float x,
        float y,
        float z
    ) {
        const Vec4 position =
            object.getModelMatrix() *
            Vec4{0.0f, 0.0f, 0.0f, 1.0f};

        assert(nearlyEqual(position.x, x));
        assert(nearlyEqual(position.y, y));
        assert(nearlyEqual(position.z, z));
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
    scene.add(MeshInstance{mesh});

    Transform placement;
    placement.position = Vec3{0.0f, 0.0f, -6.0f};
    placement.rotation.z = std::numbers::pi_v<float> / 2.0f;

    const auto instance = scene.addModel(
        model,
        placement,
        Mat4::scaling(0.5f, 0.5f, 0.5f),
        Vec3{0.0f, 0.5f, 0.0f}
    );

    assert(scene.getObjects().size() == 3);
    assert(scene.getModelInstances().size() == 1);
    assert(scene.getModelInstances()[0] == instance);

    {
        const auto& objects = scene.getObjects();
        const MeshInstance& a = objects[1];
        const MeshInstance& b = objects[2];

        assert(!objects[0].modelInstance);
        assert(a.modelInstance == instance);
        assert(b.modelInstance == instance);

        assert(a.mesh == mesh);
        assert(b.mesh == mesh);

        assert(a.material.colour.r == 10);
        assert(a.material.colour.g == 20);
        assert(a.material.colour.b == 30);
        assert(a.material.texturePath == "first.png");
        assert(!a.material.flipTextureVertically);

        assert(b.material.colour.r == 40);
        assert(b.material.embeddedImage == second.material.embeddedImage);

        assert(nearlyEqual(
            instance->initialRotation.z,
            placement.rotation.z
        ));
        assert(nearlyEqual(instance->rotationSpeed.y, 0.5f));

        assertPosition(a, 0.0f, 1.0f, -6.0f);
        assertPosition(b, 0.0f, -1.0f, -6.0f);

        // Changing one shared position moves both parts.
        instance->transform.position.x = 3.0f;

        assertPosition(a, 3.0f, 1.0f, -6.0f);
        assertPosition(b, 3.0f, -1.0f, -6.0f);

        // Shared scaling also affects both parts.
        instance->transform.scale = Vec3{2.0f, 2.0f, 2.0f};

        assertPosition(a, 3.0f, 2.0f, -6.0f);
        assertPosition(b, 3.0f, -2.0f, -6.0f);
    }

    // Another placement of the same asset has an independent transform.
    const auto other = scene.addModel(model);

    assert(other != instance);
    assert(scene.getModelInstances().size() == 2);

    other->transform.position.x = 10.0f;

    assertPosition(scene.getObjects()[3], 12.0f, 0.0f, 0.0f);
    assertPosition(scene.getObjects()[4], 8.0f, 0.0f, 0.0f);
    assertPosition(scene.getObjects()[1], 3.0f, 2.0f, -6.0f);

    // A bad part must not leave a partially added model.
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
    assert(scene.getObjects().size() == 5);
    assert(scene.getModelInstances().size() == 2);

    const auto empty = scene.addModel(Model{});

    assert(empty != nullptr);
    assert(scene.getObjects().size() == 5);
    assert(scene.getModelInstances().size() == 2);
}