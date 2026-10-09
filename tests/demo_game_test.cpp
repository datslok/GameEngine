#include "games/demo/demo_game.h"
#include "gameplay/character.h"
#include "math/transform.h"
#include "scene/camera_ray.h"
#include "scene/interpolation.h"
#include "scene/light.h"
#include "scene/lighting.h"
#include "scene/model_renderer.h"

#include <cassert>
#include <cmath>

namespace {
    bool nearlyEqual(float a, float b) {
        return std::abs(a - b) < 0.001f;
    }

    constexpr float tickSeconds = 1.0f / 120.0f;

    // An Input that looks like a focused MOBA window with a confined cursor.
    Input mobaInput() {
        Input input;
        input.beginFrame();
        input.setWindowSize(Vec2{1600.0f, 900.0f});
        input.setWindowState(true, false, true);
        input.setCursor(Vec2{800.0f, 450.0f}, true);
        return input;
    }

    void runTicks(DemoGame& game, World& world, int count, int& ticksSoFar) {
        for (int i = 0; i < count; ++i) {
            ++ticksSoFar;
            runFixedUpdate(game, world, tickSeconds, ticksSoFar * static_cast<double>(tickSeconds));
        }
    }
}

/*
* The phase 3 milestone: the whole demo simulation runs without a window or GPU, driven only by World, Input and fixed ticks.
*/
void testDemoGame() {
    World world;
    AssetManager assets;
    DemoGame game{ControlMode::Moba, false};
    game.onInit(world, assets);

    const Entity player = game.getPlayer();
    const Entity marker = game.getDestinationMarker();

    assert(world.isAlive(player));
    assert(world.isAlive(marker));
    assert(!game.wantsMouseLook());
    assert(!world.get<ModelRenderer>(marker).visible);

    // Files are loaded once: the two cubes share a mesh, so there are fewer meshes than drawable parts.
    std::size_t parts = 0;
    world.each<ModelRenderer>([&](Entity, ModelRenderer& renderer) {
        parts += renderer.parts.size();
    });
    assert(assets.getMeshCount() < parts);
    assert(assets.getTextureCount() >= 1);

    const Vec3 start = world.get<Transform>(player).position;

    // A right-click three quarters across the window, released in the same frame.
    Input input = mobaInput();
    input.pressMouseButton(MouseButtonPress{MouseButton::Right, Vec2{1200.0f, 450.0f}, 0});
    input.releaseMouseButton(MouseButton::Right);
    game.onInput(world, input);

    // Where that click should land on the ground, worked out independently.
    const auto expected = intersectGround(makeCameraRay(game.getCamera(), 0.75f, 0.5f, 1600.0f / 900.0f));
    assert(expected);
    assert(world.get<ModelRenderer>(marker).visible);
    assert(world.get<CharacterMovement>(player).isMoving());

    // Nothing moves until a tick runs: input only becomes a command.
    assert(nearlyEqual(world.get<Transform>(player).position.x, start.x));

    int ticks = 0;
    runTicks(game, world, 1000, ticks);

    const Vec3 end = world.get<Transform>(player).position;
    assert(nearlyEqual(end.x, expected->x));
    assert(nearlyEqual(end.z, expected->z));
    assert(!world.get<CharacterMovement>(player).isMoving());

    // On arrival the simulation hides the marker.
    assert(!world.get<ModelRenderer>(marker).visible);

    // The torch follows the player, one unit above it, and interpolates like it.
    const Entity torch = game.getPlayerTorch();
    assert(world.isAlive(torch));
    assert(world.has<PointLight>(torch));
    assert(world.has<PreviousTransform>(torch));

    const Vec3 torchPosition = world.get<Transform>(torch).position;
    assert(nearlyEqual(torchPosition.x, end.x));
    assert(nearlyEqual(torchPosition.y, end.y + 1.0f));
    assert(nearlyEqual(torchPosition.z, end.z));

    // A tap does not start hold-to-steer: with the button up, later frames give no new command.
    Input later = mobaInput();
    later.setCursor(Vec2{100.0f, 100.0f}, true);
    game.onInput(world, later);
    assert(!world.get<CharacterMovement>(player).isMoving());

    // Without focus or a confined cursor, clicks are ignored.
    Input unfocused = mobaInput();
    unfocused.setWindowState(false, false, false);
    unfocused.pressMouseButton(MouseButtonPress{MouseButton::Right, Vec2{400.0f, 450.0f}, 0});
    game.onInput(world, unfocused);
    assert(!world.get<CharacterMovement>(player).isMoving());

    // It is night: no sun, only a faint ambient light.
    const FrameLighting night = collectLighting(world, 1.0f);
    assert(night.directionalLights.empty());
    assert(night.ambient.x < 0.1f);

    // In MOBA mode the duck carries the flashlight, pointing where it faces and tilted down at the ground.
    game.onUpdate(world, mobaInput(), 1.0f / 60.0f, 1.0f);
    const Entity flashlight = game.getFlashlight();
    assert(world.has<SpotLight>(flashlight));

    const Vec3 duckFacing = getCharacterFacing(world, player, 1.0f);
    const Vec3 beam = world.get<SpotLight>(flashlight).direction.normalized();
    const Vec3 flashlightPosition = world.get<Transform>(flashlight).position;
    assert(nearlyEqual(flashlightPosition.x, end.x) && nearlyEqual(flashlightPosition.z, end.z));
    assert(flashlightPosition.y > end.y);
    assert(beam.y < 0.0f);
    const Vec3 beamAcrossGround = Vec3{beam.x, 0.0f, beam.z}.normalized();
    assert(beamAcrossGround.dot(duckFacing) > 0.999f);

    // F switches the flashlight off and on again.
    Input pressF = mobaInput();
    pressF.pressKey(Key::F);
    game.onInput(world, pressF);
    assert(!world.has<SpotLight>(flashlight));
    game.onInput(world, pressF);
    assert(world.has<SpotLight>(flashlight));

    // The free camera mode asks the engine for mouse look.
    DemoGame freeCamera{ControlMode::FreeCamera, false};
    World otherWorld;
    AssetManager otherAssets;
    freeCamera.onInit(otherWorld, otherAssets);
    assert(freeCamera.wantsMouseLook());

    // Away from MOBA mode, the flashlight is held at the camera and points where it looks.
    Input idle;
    idle.beginFrame();
    freeCamera.onUpdate(otherWorld, idle, 1.0f / 60.0f, 1.0f);
    const Entity cameraFlashlight = freeCamera.getFlashlight();
    const Vec3 heldAt = otherWorld.get<Transform>(cameraFlashlight).position;
    const Vec3 cameraPosition = freeCamera.getCamera().getPosition();
    assert(nearlyEqual(heldAt.x, cameraPosition.x) && nearlyEqual(heldAt.y, cameraPosition.y) && nearlyEqual(heldAt.z, cameraPosition.z));
    assert(otherWorld.get<SpotLight>(cameraFlashlight).direction.normalized().dot(freeCamera.getCamera().getForward()) > 0.999f);
}
