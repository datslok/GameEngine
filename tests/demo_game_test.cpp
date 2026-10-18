#include "engine/engine_settings.h"
#include "games/demo/demo_game.h"
#include "gameplay/character.h"
#include "gameplay/firefly.h"
#include "gameplay/first_person_body.h"
#include "gameplay/grenade.h"
#include "math/transform.h"
#include "physics/character_controller.h"
#include "physics/collider.h"
#include "render/gpu/light_uniforms.h"
#include "scene/camera_ray.h"
#include "scene/interpolation.h"
#include "scene/light.h"
#include "scene/lighting.h"
#include "scene/model_renderer.h"
#include "demo_helpers.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>

namespace {
    bool nearlyEqual(float a, float b) {
        return std::abs(a - b) < 0.001f;
    }

    // The window pixel where a world point appears: makeCameraRay run backwards. Seen from the camera, the point lies along
    // forward + right * x + up * y with x and y in units of distance ahead; the projection's [1][1] = cot(fov / 2) and the
    // aspect ratio turn those into -1..1 screen coordinates, and the window size into pixels (y counting down).
    Vec2 pixelOf(const Camera& camera, const Vec3& point, const Vec2& windowSize) {
        const Vec3 toPoint = point - camera.getPosition();
        const Vec3 forward = camera.getForward();
        const Vec3 right = camera.getRight();
        const Vec3 screenUp = right.cross(forward).normalized();
        const float ahead = toPoint.dot(forward);
        const float verticalScale = camera.getProjectionMatrix().values[1][1];
        const float aspect = windowSize.x / windowSize.y;
        const float ndcX = toPoint.dot(right) / ahead * verticalScale / aspect;
        const float ndcY = toPoint.dot(screenUp) / ahead * verticalScale;
        return Vec2{(ndcX + 1.0f) * 0.5f * windowSize.x, (1.0f - ndcY) * 0.5f * windowSize.y};
    }

    void leftClick(DemoGame& game, World& world, const Vec2& pixel) {
        Input click = mobaInput();
        click.pressMouseButton(MouseButtonPress{MouseButton::Left, pixel, 0});
        click.releaseMouseButton(MouseButton::Left);
        game.onInput(world, click);
    }

    // In MOBA mode a left-click selects whatever collider is under the cursor (the nearest one), and a click on bare
    // ground clears the selection. The selection is outlined even with the debug view off.
    void testSelection() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::Moba, false};
        game.onInit(world, assets);
        assert(game.getSelected() == Entity{});

        const Vec2 window{1600.0f, 900.0f};
        const Camera& camera = game.getCamera();

        leftClick(game, world, pixelOf(camera, getCharacterVisualCentre(world, game.getPlayer(), 1.0f), window));
        assert(game.getSelected() == game.getPlayer());

        DebugDraw outline;
        game.onDebugDraw(world, outline, 1.0f);
        assert(!game.wantsDebugView());
        assert(!outline.getLines().empty());

        // A spinning cube is picked by its triangles: the click at its centre selects it, not the duck.
        leftClick(game, world, pixelOf(camera, Vec3{-3.0f, 0.0f, -10.0f}, window));
        const Entity cube = game.getSelected();
        assert(world.isAlive(cube));
        assert(!(cube == game.getPlayer()));
        assert(world.get<Collider>(cube).detail != nullptr);

        // Bare ground between them: nothing is selected, and nothing is outlined.
        leftClick(game, world, pixelOf(camera, Vec3{3.0f, 0.0f, -4.0f}, window));
        assert(game.getSelected() == Entity{});
        DebugDraw empty;
        game.onDebugDraw(world, empty, 1.0f);
        assert(empty.getLines().empty());
    }

    // In MOBA mode whatever is under the cursor is "hovered" every frame (outlined faintly), found through the broad phase.
    void testHover() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::Moba, false};
        game.onInit(world, assets);
        assert(game.getHovered() == Entity{});

        const Vec2 window{1600.0f, 900.0f};
        const Vec2 overDuck = pixelOf(game.getCamera(), getCharacterVisualCentre(world, game.getPlayer(), 1.0f), window);

        Input hoverDuck = mobaInput();
        hoverDuck.setCursor(overDuck, true);
        game.onUpdate(world, hoverDuck, 1.0f / 120.0f, 1.0f);
        assert(game.getHovered() == game.getPlayer());

        // Hovering is not selecting.
        assert(game.getSelected() == Entity{});
        DebugDraw outline;
        game.onDebugDraw(world, outline, 1.0f);
        assert(!outline.getLines().empty());

        // Over bare ground: nothing.
        Input hoverGround = mobaInput();
        hoverGround.setCursor(pixelOf(game.getCamera(), Vec3{3.0f, 0.0f, -4.0f}, window), true);
        game.onUpdate(world, hoverGround, 1.0f / 120.0f, 1.0f);
        assert(game.getHovered() == Entity{});
    }

    // The obstacle course is static level geometry: hovering it finds a level triangle, not an entity, and outlines it.
    void testHoverLevel() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::Moba, false};
        game.onInit(world, assets);
        assert(game.getLevel().getTriangleCount() > 0);
        assert(!game.getHoveredTriangle().has_value());

        // Slide the MOBA camera over the course (it does not reset itself), then point at the floor slab's top.
        Camera& camera = game.getCamera();
        camera.setPosition(camera.getPosition() + Vec3{17.0f, 0.0f, 4.0f});
        const Vec2 window{1600.0f, 900.0f};
        const Vec3 slabTop = obstacleCourseOrigin + Vec3{0.0f, obstacleCourseFloorTop, 0.0f};

        Input hover = mobaInput();
        hover.setCursor(pixelOf(camera, slabTop, window), true);
        game.onUpdate(world, hover, 1.0f / 120.0f, 1.0f);

        assert(game.getHovered() == Entity{});
        const std::optional<std::uint32_t> triangle = game.getHoveredTriangle();
        assert(triangle.has_value());

        // It is a floor triangle facing up, at the slab's height.
        const auto corners = game.getLevel().getTriangle(*triangle);
        const Vec3 normal = (corners[1] - corners[0]).cross(corners[2] - corners[0]).normalized();
        assert(std::abs(normal.y) > 0.99f);
        assert(nearlyEqual(corners[0].y, obstacleCourseFloorTop));

        // Level triangles are outlined only in the debug view (the ground is two huge triangles, always under the cursor).
        DebugDraw plain;
        game.onDebugDraw(world, plain, 1.0f);
        assert(plain.getLines().empty());

        Input pressF4 = mobaInput();
        pressF4.pressKey(Key::F4);
        game.onInput(world, pressF4);
        DebugDraw outline;
        game.onDebugDraw(world, outline, 1.0f);
        assert(!outline.getLines().empty());
    }

    // The duck walks on the level through the character controller: sent in front of the course's stairs, then up them
    // onto the platform, it ends up standing on top.
    void testDuckClimbsCourse() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::Moba, false};
        game.onInit(world, assets);
        const Entity duck = game.getPlayer();
        int ticks = 0;

        world.get<CharacterMovement>(duck).moveTo(obstacleCourseOrigin + Vec3{-2.0f, 0.0f, -0.5f});
        runTicks(game, world, 600, ticks);
        assert(!world.get<CharacterMovement>(duck).isMoving());
        assert(nearlyEqual(world.get<Transform>(duck).position.y, obstacleCourseFloorTop));

        world.get<CharacterMovement>(duck).moveTo(obstacleCourseOrigin + Vec3{-2.0f, 0.0f, 4.5f});
        runTicks(game, world, 600, ticks);
        assert(!world.get<CharacterMovement>(duck).isMoving());
        assert(nearlyEqual(world.get<Transform>(duck).position.y, obstacleCoursePlatformTop));
        assert(world.get<CharacterController>(duck).grounded);
    }

    // First person is a body walking through the level: holding W walks it forward at Overwatch's 5.5 m/s, the camera
    // rides at its eyes, and Space jumps it off the ground.
    void testFirstPersonWalking() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::FirstPerson, false};
        game.onInit(world, assets);
        const Entity body = game.getFirstPersonBody();
        assert(world.isAlive(body));
        const Vec3 start = world.get<Transform>(body).position;
        int ticks = 0;

        for (int frame = 0; frame < 120; ++frame) {
            runFrame(game, world, firstPersonInput({Key::W}), ticks);
        }
        const Vec3 walked = world.get<Transform>(body).position - start;
        assert(std::abs(walked.z + 5.5f) < 0.2f && std::abs(walked.x) < 1e-3f);
        const Vec3 eye = firstPersonEyePosition(world, body, 1.0f);
        assert(nearlyEqual(game.getCamera().getPosition().x, eye.x));
        assert(nearlyEqual(game.getCamera().getPosition().y, eye.y));
        assert(nearlyEqual(game.getCamera().getPosition().z, eye.z));

        runFrame(game, world, firstPersonInput({Key::Space}), ticks);
        runFrame(game, world, firstPersonInput({}), ticks);
        assert(!world.get<CharacterController>(body).grounded);
        assert(world.get<Transform>(body).position.y > start.y);
    }

    // F1 from the free camera drops the body where the camera is, and it falls onto whatever is below: here the course.
    void testDropIntoFirstPerson() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::FreeCamera, true};
        game.onInit(world, assets);
        game.getCamera().setPosition(obstacleCourseOrigin + Vec3{0.0f, 6.0f, 0.0f});

        Input pressF1;
        pressF1.beginFrame();
        pressF1.setWindowState(true, false, false);
        pressF1.pressKey(Key::F1);
        game.onUpdate(world, pressF1, tickSeconds, 1.0f);

        const Entity body = game.getFirstPersonBody();
        assert(world.get<Transform>(body).position.y > 4.0f);
        int ticks = 0;
        for (int frame = 0; frame < 240; ++frame) {
            runFrame(game, world, firstPersonInput({}), ticks);
        }
        assert(world.get<CharacterController>(body).grounded);
        assert(nearlyEqual(world.get<Transform>(body).position.y, obstacleCourseFloorTop));
    }

    // Floodlights over the course: four shadow-casting spotlights on poles at its corners, high up, each beam's centre line
    // landing on the course; their lamp heads glow and do not block their own light. The poles are part of the level.
    // K switches the lights (and heads) off and on again; the poles stay.
    void testFloodlights() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::Moba, false};
        game.onInit(world, assets);

        const std::vector<Entity> lights = game.getFloodlights();
        assert(lights.size() == floodlightCount);
        for (const Entity light : lights) {
            const SpotLight& beam = world.get<SpotLight>(light);
            assert(beam.castsShadows);
            assert(beam.direction.y < 0.0f);
            assert(!world.get<ModelRenderer>(light).castsShadows);

            const Vec3 head = world.get<Transform>(light).position;
            assert(head.y > obstacleCourseFloorTop + 7.0f);
            const std::optional<LevelHit> landing = raycast(game.getLevel(), Ray{head, beam.direction});
            assert(landing.has_value());
            const Vec3 spot = landing->hit.point - obstacleCourseOrigin;
            assert(std::abs(spot.x) < 3.0f && std::abs(spot.z) < 8.0f);
        }

        // Walking sideways into a pole from 2 to its left, at knee height, meets it just before its centre.
        for (const Vec3& foot : floodlightPoleFeet()) {
            const std::optional<LevelHit> pole = raycast(game.getLevel(), Ray{foot + Vec3{-2.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}});
            assert(pole.has_value());
            assert(pole->hit.distance > 1.5f && pole->hit.distance < 2.0f);
        }

        Input pressK = mobaInput();
        pressK.pressKey(Key::K);
        game.onInput(world, pressK);
        assert(game.getFloodlights().empty());
        for (const Entity light : lights) {
            assert(!world.isAlive(light));
        }
        assert(raycast(game.getLevel(), Ray{floodlightPoleFeet()[0] + Vec3{-2.0f, 1.0f, 0.0f}, Vec3{1.0f, 0.0f, 0.0f}}).has_value());

        Input pressAgain = mobaInput();
        pressAgain.pressKey(Key::K);
        game.onInput(world, pressAgain);
        assert(game.getFloodlights().size() == floodlightCount);
    }

    // A right-click on the platform aims at the platform itself (the level), not at the ground plane beneath it: the
    // marker appears on top of it.
    void testClickOnPlatform() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::Moba, false};
        game.onInit(world, assets);

        Camera& camera = game.getCamera();
        camera.setPosition(camera.getPosition() + Vec3{17.0f, 0.0f, 4.0f});
        const Vec3 onPlatform = obstacleCourseOrigin + Vec3{-2.0f, obstacleCoursePlatformTop, 4.5f};

        Input click = mobaInput();
        click.pressMouseButton(MouseButtonPress{MouseButton::Right, pixelOf(camera, onPlatform, Vec2{1600.0f, 900.0f}), 0});
        click.releaseMouseButton(MouseButton::Right);
        game.onInput(world, click);

        const Vec3 marker = world.get<Transform>(game.getDestinationMarker()).position;
        assert(std::abs(marker.x - onPlatform.x) < 0.01f && std::abs(marker.z - onPlatform.z) < 0.01f);
        assert(marker.y > obstacleCoursePlatformTop);
        assert(marker.y < obstacleCoursePlatformTop + 0.1f);
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
        assert(!game.wantsDebugView());
        assert(debug.getLines().empty());

        Input pressF4 = mobaInput();
        pressF4.pressKey(Key::F4);
        game.onInput(world, pressF4);
        assert(game.wantsDebugView());

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
        assert(!game.wantsDebugView());

        DebugDraw off;
        game.onDebugDraw(world, off, 1.0f);
        assert(off.getLines().empty());
    }

    std::size_t countPointLights(World& world) {
        std::size_t count = 0;
        world.each<PointLight>([&](Entity, PointLight&) {
            ++count;
        });
        return count;
    }

    // L fills the ground with more lamps than the shader has seats, to show light priority at work, and removes them
    // again, entities and all.
    void testLampsToggle() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::Moba, false};
        game.onInit(world, assets);

        const std::size_t entitiesBefore = world.getEntityCount();
        assert(countPointLights(world) == 0);

        Input pressL = mobaInput();
        pressL.pressKey(Key::L);
        game.onInput(world, pressL);
        assert(countPointLights(world) == 100);
        assert(countPointLights(world) > static_cast<std::size_t>(maxPointLights)); // more than the seats, so some must wait

        // Every lamp is drawn too: a glowing bulb that does not block its own light.
        std::size_t bulbs = 0;
        world.each<PointLight, ModelRenderer>([&](Entity, PointLight&, ModelRenderer& renderer) {
            assert(!renderer.castsShadows);
            ++bulbs;
        });
        assert(bulbs == countPointLights(world));

        game.onInput(world, pressL);
        assert(countPointLights(world) == 0);
        assert(world.getEntityCount() == entitiesBefore);
    }

    // The lamps fly round their pillars like fireflies, so the shadows sweep. Switched on part way through the game, each
    // starts exactly where its flight puts it at that moment, so the first tick does not make it jump.
    void testLampsFly() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::Moba, false};
        game.onInit(world, assets);

        int ticks = 0;
        runTicks(game, world, 500, ticks);

        Input pressL = mobaInput();
        pressL.pressKey(Key::L);
        game.onInput(world, pressL);

        const double now = ticks * static_cast<double>(tickSeconds);
        std::size_t flying = 0;
        world.each<Firefly, PointLight>([&](Entity entity, Firefly& firefly, PointLight&) {
            assert(world.has<PreviousTransform>(entity));
            const Vec3 expected = fireflyPosition(firefly, now);
            const Vec3 position = world.get<Transform>(entity).position;
            assert(nearlyEqual(position.x, expected.x) && nearlyEqual(position.y, expected.y) && nearlyEqual(position.z, expected.z));
            ++flying;
        });
        assert(flying == countPointLights(world));

        const Entity someLamp = [&] {
            Entity found;
            world.each<Firefly>([&](Entity entity, Firefly&) {
                found = entity;
            });
            return found;
        }();
        const Vec3 before = world.get<Transform>(someLamp).position;

        runTicks(game, world, 120, ticks);
        assert((world.get<Transform>(someLamp).position - before).length() > 0.1f);
    }

    std::size_t countGrenades(World& world) {
        std::size_t count = 0;
        world.each<Grenade>([&](Entity, Grenade&) {
            ++count;
        });
        return count;
    }

    // In first person, G throws a grenade from the eyes along the view; it flies off forward and falls, and disappears when
    // its lifetime is over. Other modes do not throw.
    void testThrowGrenade() {
        World world;
        AssetManager assets;
        DemoGame game{ControlMode::FirstPerson, true};
        game.onInit(world, assets);
        int ticks = 0;
        runFrame(game, world, firstPersonInput({}), ticks);
        assert(countGrenades(world) == 0);

        const Vec3 eye = game.getCamera().getPosition();
        const Vec3 forward = game.getCamera().getForward();
        runFrame(game, world, firstPersonInput({Key::G}), ticks);
        assert(countGrenades(world) == 1);
        Entity grenade;
        world.each<Grenade>([&](Entity entity, Grenade&) {
            grenade = entity;
        });
        assert((world.get<Transform>(grenade).position - eye).length() < 0.5f);

        for (int frame = 0; frame < 30; ++frame) {
            runFrame(game, world, firstPersonInput({}), ticks);
        }
        const Vec3 flown = world.get<Transform>(grenade).position - eye;
        assert(flown.dot(forward) > 2.0f);
        assert(world.get<Particle>(grenade).velocity.y < 0.0f);

        // Each fresh press throws another.
        runFrame(game, world, firstPersonInput({Key::G}), ticks);
        assert(countGrenades(world) == 2);

        const int lifetimeFrames = static_cast<int>(GrenadeSettings{}.lifetimeSeconds / tickSeconds) + 2;
        for (int frame = 0; frame < lifetimeFrames; ++frame) {
            runFrame(game, world, firstPersonInput({}), ticks);
        }
        assert(countGrenades(world) == 0);

        World mobaWorld;
        AssetManager mobaAssets;
        DemoGame moba{ControlMode::Moba, false};
        moba.onInit(mobaWorld, mobaAssets);
        Input pressG = mobaInput();
        pressG.pressKey(Key::G);
        moba.onInput(mobaWorld, pressG);
        assert(countGrenades(mobaWorld) == 0);
    }
}

/*
* The phase 3 milestone: the whole demo simulation runs without a window or GPU, driven only by World, Input and fixed ticks.
*/
void testDemoGame() {
    testDebugViewToggles();
    testSelection();
    testHover();
    testHoverLevel();
    testDuckClimbsCourse();
    testClickOnPlatform();
    testFloodlights();
    testFirstPersonWalking();
    testDropIntoFirstPerson();
    testLampsToggle();
    testLampsFly();
    testThrowGrenade();

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
