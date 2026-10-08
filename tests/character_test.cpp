#include "gameplay/character.h"
#include "assets/gltf_loader.h"
#include "math/transform.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"

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

    // The lowest vertex of the drawn model must touch the given height.
    void assertGrounded(const World& world, const AssetManager& assets, Entity character, float height) {
        const Mat4 entityMatrix = world.get<Transform>(character).getMatrix();
        float bottom = std::numeric_limits<float>::infinity();

        for (const RenderPart& part : world.get<ModelRenderer>(character).parts) {
            for (const Vec4& vertex : assets.getMesh(part.mesh).vertices) {
                bottom = std::min(bottom, (entityMatrix * part.localTransform * vertex).y);
            }
        }
        assert(nearlyEqual(bottom, height));
    }

    void assertFacing(const World& world, Entity character, float localForwardYaw,
                      const Vec3& expected) {
        const float yaw = world.get<Transform>(character).rotation.y;
        const Vec4 forward = Mat4::rotationY(yaw) * Vec4{
            std::sin(localForwardYaw), 0.0f, std::cos(localForwardYaw), 0.0f
        };
        assert(nearlyEqual(forward.x, expected.x));
        assert(nearlyEqual(forward.z, expected.z));
    }

    const Vec3& position(const World& world, Entity character) {
        return world.get<Transform>(character).position;
    }

    float yaw(const World& world, Entity character) {
        return world.get<Transform>(character).rotation.y;
    }
}

void testCharacter() {
    // One manager for the whole test: characters made from the same model share its mesh handles.
    AssetManager assets;

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
            World world;
            CharacterConfig config;
            config.modelForwardYaw = forwardYaw;
            const Entity character = spawnCharacter(world, assets, cube, config, Vec3{0, 3, 0});
            CharacterMovement& movement = world.get<CharacterMovement>(character);

            assertGrounded(world, assets, character, 3.0f);
            assert(nearlyEqual(getCharacterVisualCentre(world, character, 1.0f).y, 4.0f));
            movement.moveTo(direction * 12.0f);
            updateCharacters(world, 0.5f);
            assert(nearlyEqual(position(world, character).x, direction.x * 3));
            assert(nearlyEqual(position(world, character).z, direction.z * 3));
            assert(nearlyEqual(position(world, character).y, 3.0f));
            assert(movement.isMoving());
            assertFacing(world, character, forwardYaw, direction);
            assertGrounded(world, assets, character, 3.0f);

            updateCharacters(world, 10.0f);
            assert(!movement.isMoving());
            assert(nearlyEqual(position(world, character).x, direction.x * 12));
            assert(nearlyEqual(position(world, character).z, direction.z * 12));
        }
    }

    // Arrival does not prematurely cancel a slow turn.
    World world;
    CharacterConfig slowTurn;
    slowTurn.turnSpeed = pi;
    const Entity character = spawnCharacter(world, assets, cube, slowTurn, Vec3{0, 0, 0});
    world.get<CharacterMovement>(character).moveTo(Vec3{0.01f, 0, 0});
    updateCharacters(world, 0.01f);
    assert(!world.get<CharacterMovement>(character).isMoving());
    assert(nearlyEqual(yaw(world, character), pi * 0.01f));
    updateCharacters(world, 1.0f);
    assertFacing(world, character, 0, Vec3{1, 0, 0});

    // Turning crosses the +/-pi boundary by the shortest route.
    world.get<Transform>(character).rotation.y = pi - 0.05f;
    const float desiredYaw = -pi + 0.05f;
    world.get<CharacterMovement>(character).moveTo(position(world, character) + Vec3{
        std::sin(desiredYaw), 0, std::cos(desiredYaw)
    });
    updateCharacters(world, 0.01f);
    assert(nearlyEqual(yaw(world, character), pi - 0.05f + pi * 0.01f));

    // Explicit stop cancels both travel and any remaining turn.
    world.get<CharacterMovement>(character).stop();
    const Vec3 stoppedPosition = position(world, character);
    const float stoppedYaw = yaw(world, character);
    updateCharacters(world, 1.0f);
    assert(!world.get<CharacterMovement>(character).isMoving());
    assert(nearlyEqual(position(world, character).x, stoppedPosition.x));
    assert(nearlyEqual(position(world, character).z, stoppedPosition.z));
    assert(nearlyEqual(yaw(world, character), stoppedYaw));

    // Two characters can share a loaded asset without sharing movement state.
    const Entity other = spawnCharacter(world, assets, cube, CharacterConfig{}, Vec3{10, 0, 0});
    world.get<CharacterMovement>(other).moveTo(Vec3{16, 0, 0});
    updateCharacters(world, 0.5f);
    assert(nearlyEqual(position(world, other).x, 13));
    assert(nearlyEqual(position(world, character).x, stoppedPosition.x));
    assert(world.get<ModelRenderer>(character).parts[0].mesh ==
           world.get<ModelRenderer>(other).parts[0].mesh);

    // The camera target is blended between the last two ticks.
    world.get<PreviousTransform>(other).transform.position.x = 12.0f;
    assert(nearlyEqual(getCharacterVisualCentre(world, other, 0.5f).x, 12.5f));

    // Real duck: grounding preserves its old visible placement and camera aim.
    const Model duck = loadGltf("assets/models/Duck.glb");
    World duckWorld;
    CharacterConfig duckConfig;
    duckConfig.modelForwardYaw = pi / 2;
    const Entity duckCharacter = spawnCharacter(duckWorld, assets, duck, duckConfig, Vec3{0, 0, -6});
    const Mat4 normalization = duck.getNormalizationMatrix(2);
    const Vec3 minimum = duck.getBounds().minimum;
    const float oldHeight = -(normalization * Vec4{
        minimum.x, minimum.y, minimum.z, 1
    }).y;
    assert(nearlyEqual(position(duckWorld, duckCharacter).y, 0));
    assert(nearlyEqual(getCharacterVisualCentre(duckWorld, duckCharacter, 1.0f).y, oldHeight));
    assertGrounded(duckWorld, assets, duckCharacter, 0);
    duckWorld.get<CharacterMovement>(duckCharacter).moveTo(Vec3{0, 0, 0});
    updateCharacters(duckWorld, 1);
    assertFacing(duckWorld, duckCharacter, pi / 2, Vec3{0, 0, 1});
    assertGrounded(duckWorld, assets, duckCharacter, 0);

    // Bad configuration must not leave a partial character in the world.
    for (int field = 0; field < 5; ++field) {
        World invalidWorld;
        CharacterConfig bad;
        Vec3 start{0, 0, 0};
        if (field == 0) bad.movementSpeed = 0;
        if (field == 1) bad.turnSpeed = -1;
        if (field == 2) bad.modelSize = 0;
        if (field == 3) bad.modelForwardYaw = std::numeric_limits<float>::infinity();
        if (field == 4) start.y = std::numeric_limits<float>::quiet_NaN();
        bool rejected = false;
        try {
            spawnCharacter(invalidWorld, assets, cube, bad, start);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        assert(rejected);
        assert(invalidWorld.getEntityCount() == 0);
    }
}
