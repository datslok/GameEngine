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

namespace {
    // F4 switches the debug view: the engine's bounding boxes, plus the game's own shapes (a marker at each light, and
    // in MOBA mode a line to where the duck is walking). Off, nothing extra is drawn.
    void testDebugViewToggles() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::Moba, false};
        game.onInit(world, assets);

        DebugDraw debug;
        game.onDebugDraw(world, debug, 1.0f);
        assert(!game.wantsBoundingBoxes());
        assert(debug.getLines().empty());

        Input pressF4 = mobaInput();
        pressF4.pressKey(Key::F4);
        game.onInput(world, pressF4);
        assert(game.wantsBoundingBoxes());

        game.onDebugDraw(world, debug, 1.0f);
        const std::size_t withLights = debug.getLines().size();
        assert(withLights > 0);

        // Once the duck is told to walk, a line shows where to.
        Input click = mobaInput();
        click.pressMouseButton(MouseButtonPress{MouseButton::Right, Vec2{400.0f, 450.0f}, 0});
        game.onInput(world, click);
        assert(world.get<CharacterMovement>(game.getPlayer()).isMoving());

        DebugDraw walking;
        game.onDebugDraw(world, walking, 1.0f);
        assert(walking.getLines().size() == withLights + 1);

        game.onInput(world, pressF4);
        assert(!game.wantsBoundingBoxes());

        DebugDraw off;
        game.onDebugDraw(world, off, 1.0f);
        assert(off.getLines().empty());
    }
}

/*
* The phase 3 milestone: the whole demo simulation runs without a window or GPU, driven only by World, Input and fixed ticks.
*/
void testDemoGame() {
    testDebugViewToggles();

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

    // It is night: a faint ambient light and a weak, cool moon shining down from above, as a directional light (parallel
    // rays, like the real moon) that casts shadows. No point light stands in for it any more.
    const FrameLighting night = collectLighting(world, 1.0f);
    assert(night.ambient.x < 0.1f);
    assert(night.directionalLights.size() == 1);
    assert(night.pointLights.empty());

    const DirectionalLight& moonlight = night.directionalLights[0];
    assert(moonlight.intensity > 0.0f && moonlight.intensity <= 0.2f);
    assert(moonlight.colour.z > moonlight.colour.x);
    assert(moonlight.direction.y < 0.0f);
    assert(moonlight.castsShadows);

    // In MOBA mode the duck carries the flashlight, pointing where it faces and tilted down at the ground.
    game.onUpdate(world, mobaInput(), 1.0f / 60.0f, 1.0f);
    const Entity flashlight = game.getFlashlight();
    assert(world.has<SpotLight>(flashlight));

    const Vec3 duckFacing = getCharacterFacing(world, player, 1.0f);
    const Vec3 beam = world.get<SpotLight>(flashlight).direction.normalized();
    const Vec3 flashlightPosition = world.get<Transform>(flashlight).position;
    // Held out in front of the duck, beyond its body (the model is 2 units long), so the duck does not block its own light.
    const Vec3 heldOut = flashlightPosition - end;
    const float reach = Vec3(heldOut.x, 0.0f, heldOut.z).length();
    assert(reach > 1.0f && reach < 1.5f);
    const Vec3 heldOutAcrossGround{heldOut.x, 0.0f, heldOut.z};
    assert(heldOutAcrossGround.normalized().dot(duckFacing) > 0.999f);
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

    // Away from MOBA mode, the flashlight is held in the right hand, a little to the right of and below the eye, and aimed
    // at the middle of the view. Being off to the side, its shadows show beside things instead of hiding behind them.
    Input idle;
    idle.beginFrame();
    freeCamera.onUpdate(otherWorld, idle, 1.0f / 60.0f, 1.0f);
    const Entity cameraFlashlight = freeCamera.getFlashlight();
    const Vec3 heldAt = otherWorld.get<Transform>(cameraFlashlight).position;
    const Camera& view = freeCamera.getCamera();
    const Vec3 fromEye = heldAt - view.getPosition();
    assert(fromEye.dot(view.getRight()) > 0.2f && fromEye.dot(view.getRight()) < 0.6f);
    assert(fromEye.dot(view.getUp()) < -0.1f && fromEye.dot(view.getUp()) > -0.5f);

    // The beam passes through the point straight ahead of the eye, where the crosshair would be.
    const Vec3 beamDirection = otherWorld.get<SpotLight>(cameraFlashlight).direction.normalized();
    const Vec3 aimPoint = view.getPosition() + view.getForward() * 8.0f;
    const Vec3 towardsAim = (aimPoint - heldAt).normalized();
    assert(beamDirection.dot(towardsAim) > 0.9999f);

    // The glowing moon is a fixed place you can fly to, high in the sky,
    // 64 units from where the free camera starts (about 20 seconds of flying at 3 units per second, minus its radius of 4).
    // It sits where the moonlight comes from, opposite the way the light travels.
    const Entity moonBall = freeCamera.getMoon();
    const Vec3 freeCameraStart{2.0f, 1.0f, 0.0f};
    const Vec3 expectedMoon = otherWorld.get<Transform>(moonBall).position;
    assert(nearlyEqual((expectedMoon - freeCameraStart).length(), 64.0f));
    assert(expectedMoon.y > 30.0f);

    const Vec3 moonlightDirection = collectLighting(otherWorld, 1.0f).directionalLights[0].direction.normalized();
    assert((expectedMoon - freeCameraStart).normalized().dot(moonlightDirection) < -0.999f);

    // It stays put when the camera moves, so flying towards it gets you there.
    freeCamera.getCamera().setPosition(Vec3{30.0f, 20.0f, -30.0f});
    freeCamera.onUpdate(otherWorld, idle, 1.0f / 60.0f, 1.0f);
    const Vec3 afterMoving = otherWorld.get<Transform>(moonBall).position;
    assert(nearlyEqual(afterMoving.x, expectedMoon.x) && nearlyEqual(afterMoving.y, expectedMoon.y) && nearlyEqual(afterMoving.z, expectedMoon.z));

    const Material& moonMaterial = otherWorld.get<ModelRenderer>(moonBall).parts[0].material;
    assert(moonMaterial.emissive.z > 0.5f);

    // The sphere surrounds its own light, so it must not block it.
    assert(!otherWorld.get<ModelRenderer>(moonBall).castsShadows);
}
