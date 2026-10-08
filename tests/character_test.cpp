#include "character.h"
#include "gltf_loader.h"
#include "scene.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace {
    constexpr float pi = std::numbers::pi_v<float>;

    bool nearlyEqual(float a, float b) {
        return std::abs(a - b) < 0.0001f;
    }

    void assertGrounded(const Scene& scene, float height) {
        float bottom = std::numeric_limits<float>::infinity();
        for (const MeshInstance& part : scene.getObjects()) {
            for (const Vec4& vertex : part.mesh->vertices) {
                bottom = std::min(bottom, (part.getModelMatrix() * vertex).y);
            }
        }
        assert(nearlyEqual(bottom, height));
    }

    void assertFacing(const Scene& scene, float localForwardYaw,
                      const Vec3& expected) {
        const float yaw = scene.getModelInstances().front()->transform.rotation.y;
        const Vec4 forward = Mat4::rotationY(yaw) * Vec4{
            std::sin(localForwardYaw), 0.0f, std::cos(localForwardYaw), 0.0f
        };
        assert(nearlyEqual(forward.x, expected.x));
        assert(nearlyEqual(forward.z, expected.z));
    }
}

void testCharacter() {
    Model cube;
    ModelPart part;
    part.mesh = std::make_shared<Mesh>(Mesh::cube());
    // Exercise imported node translation and non-uniform scale.
    part.transform = Mat4::translation(3.0f, -7.0f, 4.0f) *
                     Mat4::scaling(1.0f, 2.0f, 1.0f);
    cube.parts.push_back(part);

    // Both +Z-facing and +X-facing models follow the same four commands.
    for (float forwardYaw : {0.0f, pi / 2.0f}) {
        for (const Vec3 direction : {Vec3{1, 0, 0}, Vec3{-1, 0, 0},
                                     Vec3{0, 0, 1}, Vec3{0, 0, -1}}) {
            Scene scene;
            CharacterConfig config;
            config.modelForwardYaw = forwardYaw;
            Character character{scene, cube, config, Vec3{0, 3, 0}};

            assertGrounded(scene, 3.0f);
            assert(nearlyEqual(character.getVisualCentre().y, 4.0f));
            character.moveTo(direction * 12.0f);
            character.update(0.5f);
            assert(nearlyEqual(character.getPosition().x, direction.x * 3));
            assert(nearlyEqual(character.getPosition().z, direction.z * 3));
            assert(nearlyEqual(character.getPosition().y, 3.0f));
            assert(character.isMoving());
            assertFacing(scene, forwardYaw, direction);
            assertGrounded(scene, 3.0f);

            character.update(10.0f);
            assert(!character.isMoving());
            assert(nearlyEqual(character.getPosition().x, direction.x * 12));
            assert(nearlyEqual(character.getPosition().z, direction.z * 12));
        }
    }

    // Arrival does not prematurely cancel a slow turn.
    Scene scene;
    CharacterConfig slowTurn;
    slowTurn.turnSpeed = pi;
    Character character{scene, cube, slowTurn, Vec3{0, 0, 0}};
    character.moveTo(Vec3{0.01f, 0, 0});
    character.update(0.01f);
    assert(!character.isMoving());
    assert(nearlyEqual(scene.getModelInstances()[0]->transform.rotation.y,
                       pi * 0.01f));
    character.update(1.0f);
    assertFacing(scene, 0, Vec3{1, 0, 0});

    // Turning crosses the +/-pi boundary by the shortest route.
    scene.getModelInstances()[0]->transform.rotation.y = pi - 0.05f;
    const float desiredYaw = -pi + 0.05f;
    character.moveTo(character.getPosition() + Vec3{
        std::sin(desiredYaw), 0, std::cos(desiredYaw)
    });
    character.update(0.01f);
    assert(nearlyEqual(scene.getModelInstances()[0]->transform.rotation.y,
                       pi - 0.05f + pi * 0.01f));

    // Explicit stop cancels both travel and any remaining turn.
    character.stop();
    const Vec3 stoppedPosition = character.getPosition();
    const float stoppedYaw = scene.getModelInstances()[0]->transform.rotation.y;
    character.update(1.0f);
    assert(!character.isMoving());
    assert(nearlyEqual(character.getPosition().x, stoppedPosition.x));
    assert(nearlyEqual(character.getPosition().z, stoppedPosition.z));
    assert(nearlyEqual(scene.getModelInstances()[0]->transform.rotation.y, stoppedYaw));

    // Two characters can share a loaded asset without sharing movement state.
    Character other{scene, cube, CharacterConfig{}, Vec3{10, 0, 0}};
    other.moveTo(Vec3{16, 0, 0});
    other.update(0.5f);
    assert(nearlyEqual(other.getPosition().x, 13));
    assert(nearlyEqual(character.getPosition().x, stoppedPosition.x));
    assert(scene.getObjects()[0].mesh == scene.getObjects()[1].mesh);

    // Real duck: grounding preserves its old visible placement and camera aim.
    const Model duck = loadGltf("assets/models/Duck.glb");
    Scene duckScene;
    CharacterConfig duckConfig;
    duckConfig.modelForwardYaw = pi / 2;
    Character duckCharacter{duckScene, duck, duckConfig, Vec3{0, 0, -6}};
    const Mat4 normalization = duck.getNormalizationMatrix(2);
    const Vec3 minimum = duck.getBounds().minimum;
    const float oldHeight = -(normalization * Vec4{
        minimum.x, minimum.y, minimum.z, 1
    }).y;
    assert(nearlyEqual(duckCharacter.getPosition().y, 0));
    assert(nearlyEqual(duckCharacter.getVisualCentre().y, oldHeight));
    assertGrounded(duckScene, 0);
    duckCharacter.moveTo(Vec3{0, 0, 0});
    duckCharacter.update(1);
    assertFacing(duckScene, pi / 2, Vec3{0, 0, 1});
    assertGrounded(duckScene, 0);

    // Bad configuration must not leave a partial character in the scene.
    for (int field = 0; field < 5; ++field) {
        Scene invalidScene;
        CharacterConfig bad;
        Vec3 position{0, 0, 0};
        if (field == 0) bad.movementSpeed = 0;
        if (field == 1) bad.turnSpeed = -1;
        if (field == 2) bad.modelSize = 0;
        if (field == 3) bad.modelForwardYaw = std::numeric_limits<float>::infinity();
        if (field == 4) position.y = std::numeric_limits<float>::quiet_NaN();
        bool rejected = false;
        try {
            Character invalid{invalidScene, cube, bad, position};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
        assert(invalidScene.getObjects().empty());
        assert(invalidScene.getModelInstances().empty());
    }
}
