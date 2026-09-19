#include "scene.h"
#include "mesh.h"
#include "mesh_instance.h"

#include <cassert>
#include <memory>

void testScene() {
    Scene scene;

    // A new scene contains no objects.
    assert(scene.getObjects().empty());

    const std::shared_ptr<const Mesh> cubeMesh =
        std::make_shared<Mesh>(Mesh::cube());

    MeshInstance first{cubeMesh};
    first.transform.position = Vec3{-2.0f, 0.0f, -7.0f};
    first.colour = Pixel{220, 80, 80};
    first.initialRotation = Vec3{0.3f, 0.0f, 0.0f};
    first.rotationSpeed = Vec3{2.0f, 2.0f, 0.0f};

    MeshInstance second{cubeMesh};
    second.transform.position = Vec3{2.0f, 0.0f, -7.0f};
    second.colour = Pixel{80, 120, 220};

    scene.add(first);
    scene.add(second);

    // Both objects are stored in insertion order.
    assert(scene.getObjects().size() == 2);
    assert(scene.getObjects()[0].transform.position.x == -2.0f);
    assert(scene.getObjects()[1].transform.position.x == 2.0f);

    // Both instances share the original mesh.
    assert(scene.getObjects()[0].mesh == cubeMesh);
    assert(scene.getObjects()[1].mesh == cubeMesh);

    // Animation settings are preserved when adding an object.
    assert(scene.getObjects()[0].initialRotation.x == 0.3f);
    assert(scene.getObjects()[0].rotationSpeed.x == 2.0f);
    assert(scene.getObjects()[0].rotationSpeed.y == 2.0f);

    // Adding stores a copy: changing the original leaves the scene unchanged.
    first.transform.position.x = 99.0f;
    first.colour = Pixel{0, 0, 0};

    assert(scene.getObjects()[0].transform.position.x == -2.0f);
    assert(scene.getObjects()[0].colour.r == 220);

    // Changing one stored instance leaves the other unchanged.
    scene.getObjects()[0].transform.position.x = 5.0f;
    scene.getObjects()[0].colour = Pixel{0, 255, 0};

    assert(scene.getObjects()[1].transform.position.x == 2.0f);
    assert(scene.getObjects()[1].colour.r == 80);
    assert(scene.getObjects()[1].colour.g == 120);
    assert(scene.getObjects()[1].colour.b == 220);

    // The const accessor exposes the same stored objects.
    const Scene& readOnlyScene = scene;

    assert(readOnlyScene.getObjects().size() == 2);
    assert(readOnlyScene.getObjects()[0].transform.position.x == 5.0f);
    assert(
        &readOnlyScene.getObjects()[0] ==
        &scene.getObjects()[0]
    );
}