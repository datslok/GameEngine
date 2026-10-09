#include "games/demo/demo_game.h"
#include "gameplay/character.h"
#include "gameplay/spinner.h"
#include "math/transform.h"
#include "scene/camera_ray.h"
#include "scene/interpolation.h"
#include "scene/light.h"
#include "scene/model_renderer.h"

#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <utility>

namespace {
    // Where the duck holds the flashlight in MOBA mode, and how far it tilts the beam down (radians, about 20 degrees).
    constexpr float flashlightHeight = 1.2f;
    constexpr float flashlightTilt = 0.35f;

    // The way the moonlight travels. The moon sphere is placed the opposite way, so the light comes from where you see the moon.
    const Vec3 moonlightDirection{0.4f, -1.0f, -0.6f};

    // How far from the camera the moon is drawn (inside the camera's far plane of 100), and how big it is.
    constexpr float moonDistance = 80.0f;
    constexpr float moonRadius = 4.0f;
}

DemoGame::DemoGame(ControlMode startMode, bool debugModeSwitching):
    camera(
        Vec3{2.0f, 1.0f, 0.0f},
        Vec3{0.0f, 0.0f, -5.0f},
        Vec3{0.0f, 1.0f, 0.0f},
        70.0f * std::numbers::pi_v<float> / 180.0f, // FoV
        16.0f / 9.0f, // Replaced by the engine with the real frame's aspect ratio.
        0.1f,
        100.0f
    ),
    // Movement speed in units per second, mouse sensitivity in radians per pixel.
    firstPersonCamera(3.0f, 0.001f),
    // Camera position relative to the point it looks at, then pan speed.
    mobaCamera(Vec3{0.0f, 12.0f, 10.0f}, 9.0f),
    freeFlyCamera(3.0f, 0.001f),
    controlMode(startMode),
    enableDebugModeSwitching(debugModeSwitching)
{
}

void DemoGame::onInit(World& world, AssetManager& assets) {
    createScene(world, assets);

    // Start from the mode's predictable camera pose.
    setControlMode(world, controlMode);
}

void DemoGame::createScene(World& world, AssetManager& assets) {
    // Each file is loaded once; both cubes share one mesh handle and one texture handle.
    const MeshHandle cubeMesh = assets.addMesh(Mesh::cube());
    const MeshHandle pyramidMesh = assets.loadMesh("assets/models/pyramid.obj");
    // The teapot file has no normals; it is a curved surface, so smooth them. The pyramid's flat faces are meant to look flat.
    const MeshHandle teapotMesh = assets.loadMesh("assets/models/teapot.obj", MeshLoadOptions{.smoothNormals = true});

    // A spinning demo object: Transform + ModelRenderer + Spinner, plus PreviousTransform so it is drawn smoothly.
    const auto spawnSpinner = [&world](MeshHandle mesh, const Material& material,
                                       const Transform& transform, const Vec3& speed) {
        const Entity entity = world.create();
        world.add(entity, transform);
        world.add(entity, PreviousTransform{transform});
        world.add(entity, makeMeshRenderer(mesh, material));
        world.add(entity, Spinner{transform.rotation, speed});
    };

    Material textured;
    textured.texture = assets.loadTexture("assets/textures/demo.png");

    Material green;
    green.colour = Pixel{80, 200, 120};

    Material gold;
    gold.colour = Pixel{230, 180, 60};

    const Vec3 smallScale{0.7f, 0.7f, 0.7f};

    Transform first;
    first.position = Vec3{-3.0f, 0.0f, -10.0f};
    first.rotation.x = 0.3f;
    spawnSpinner(cubeMesh, textured, first, Vec3{2.0f, 2.0f, 0.0f});

    Transform second;
    second.position = Vec3{3.0f, 0.0f, -10.0f};
    second.scale = smallScale;
    spawnSpinner(cubeMesh, textured, second, Vec3{0.0f, -1.0f, 0.0f});

    Transform third;
    third.position = Vec3{0.0f, 3.0f, -10.0f};
    third.scale = smallScale;
    spawnSpinner(pyramidMesh, green, third, Vec3{0.0f, 1.0f, 0.0f});

    Transform fourth;
    // The teapot's origin is at its base, so y = 0 stands it on the grass. Set back so its spout clears the cubes as it spins.
    fourth.position = Vec3{0.0f, 0.0f, -13.0f};
    fourth.scale = smallScale;
    spawnSpinner(teapotMesh, gold, fourth, Vec3{0.0f, -1.0f, 0.0f});

    // Night: a faint ambient light and a weak, cool moon, so the flashlight and the glowing duck stand out.
    // Unlike ambient light, the moon comes from a direction, so shapes keep a lit side and a dark side outside the beam.
    world.add(world.create(), AmbientLight{Vec3{0.06f, 0.06f, 0.06f}});
    world.add(world.create(), DirectionalLight{moonlightDirection, Vec3{0.6f, 0.7f, 1.0f}, 0.15f});

    // The moon itself: a glowing sphere where the moonlight comes from. updateMoon keeps it at a fixed distance from the camera.
    Material moonGlow;
    moonGlow.colour = Pixel{0, 0, 0};
    moonGlow.specularStrength = 0.0f;
    moonGlow.emissive = Vec3{0.85f, 0.9f, 1.0f};

    Transform moonPlacement;
    moonPlacement.scale = Vec3{moonRadius, moonRadius, moonRadius};

    moon = world.create();
    world.add(moon, moonPlacement);
    world.add(moon, makeMeshRenderer(assets.addMesh(Mesh::sphere()), moonGlow));

    // The flashlight. updateFlashlight places it every frame, at the camera or (in MOBA mode) in the duck's hands.
    flashlight = world.create();
    world.add(flashlight, Transform{});
    world.add(flashlight, flashlightBeam);

    // Ground: it never moves, so it needs no PreviousTransform.
    Material grass;
    grass.colour = Pixel{75, 110, 75};
    grass.specularStrength = 0.0f; // Grass is matte.

    Transform groundPlacement;
    groundPlacement.position = Vec3{0.0f, 0.0f, -6.0f};

    const Entity ground = world.create();
    world.add(ground, groundPlacement);
    world.add(ground, makeMeshRenderer(assets.addMesh(Mesh::plane(20.0f)), grass));

    // Change this configuration to use another compatible static model.
    const CharacterConfig playerConfig{
        .modelPath = "assets/models/Duck.glb",
        .modelSize = 2.0f,
        .movementSpeed = 6.0f,
        .turnSpeed = 6.0f * std::numbers::pi_v<float>,
        .modelForwardYaw = std::numbers::pi_v<float> / 2.0f
    };

    const Model& model = assets.loadModel(playerConfig.modelPath);
    player = spawnCharacter(world, assets, model, playerConfig, Vec3{0.0f, 0.0f, -6.0f});

    // Movement marker. It jumps to each click instead of gliding, so it has no PreviousTransform.
    Material yellow;
    // It glows instead of reflecting light, so it is the same yellow day or night. It will become a HUD ring in phase 6.
    yellow.colour = Pixel{0, 0, 0};
    yellow.specularStrength = 0.0f;
    yellow.emissive = Vec3{1.0f, 0.86f, 0.16f};

    ModelRenderer markerRenderer = makeMeshRenderer(assets.addMesh(Mesh::plane(0.2f)), yellow);
    markerRenderer.visible = false;

    // Slightly above the ground to avoid overlapping surfaces.
    Transform markerPlacement;
    markerPlacement.position = Vec3{0.0f, 0.02f, 0.0f};

    destinationMarker = world.create();
    world.add(destinationMarker, markerPlacement);
    world.add(destinationMarker, std::move(markerRenderer));
}

void DemoGame::onInput(World& world, const Input& input) {
    // Pick using the camera pose before this frame's panning.
    updatePlayerCommands(world, input);

    if (input.wasKeyPressed(Key::F)) {
        toggleFlashlight(world);
    }
}

/*
* Switching off removes the light rather than dimming it to zero, so the renderer does not spend a slot on it.
*/
void DemoGame::toggleFlashlight(World& world) {
    if (world.has<SpotLight>(flashlight)) {
        world.remove<SpotLight>(flashlight);
    }
    else {
        world.add(flashlight, flashlightBeam);
    }
}

/*
* Runs every frame, after the camera moves, because the camera moves per frame: updating per tick would make the beam lag behind mouse look.
* In MOBA mode the duck holds it, so it uses the duck's drawn (interpolated) pose, keeping the beam in step with the model.
* It needs no PreviousTransform: it is already placed exactly where things are drawn this frame.
*/
/*
* The real moon is so far away that walking shows no parallax: it stays in the same direction and the same size.
* Keeping the sphere at a fixed offset from the camera every frame gives the same effect (the trick skyboxes use).
*/
void DemoGame::updateMoon(World& world) {
    world.get<Transform>(moon).position = camera.getPosition() - moonlightDirection.normalized() * moonDistance;
}

void DemoGame::updateFlashlight(World& world, float alpha) {
    Vec3 position = camera.getPosition();
    Vec3 direction = camera.getForward();

    if (controlMode == ControlMode::Moba && world.isAlive(player)) {
        // Held above the ground and tilted down so the beam lands a few steps ahead.
        const Vec3 facing = getCharacterFacing(world, player, alpha);
        position = getRenderTransform(world, player, alpha).position + Vec3{0.0f, flashlightHeight, 0.0f};
        direction = facing * std::cos(flashlightTilt) - Vec3{0.0f, std::sin(flashlightTilt), 0.0f};
    }

    world.get<Transform>(flashlight).position = position;

    if (SpotLight* beam = world.tryGet<SpotLight>(flashlight)) {
        beam->direction = direction;
    }
}

/*
* The systems, in a fixed order. The engine has already saved previous transforms for interpolation.
*/
void DemoGame::onFixedUpdate(World& world, float tickSeconds, double simulationSeconds) {
    updateCharacters(world, tickSeconds);
    updateSpinners(world, simulationSeconds);

    // Simulation continues when the cursor is released or focus is lost.
    if (world.isAlive(player) && !world.get<CharacterMovement>(player).isMoving()) {
        world.get<ModelRenderer>(destinationMarker).visible = false;
    }
}

/*
* The camera is the player's view, not game state, so it updates every rendered frame. Mouse look in particular must not be limited to the tick rate.
*/
void DemoGame::onUpdate(World& world, const Input& input, float frameSeconds, float alpha) {
    const bool modeChanged = updateModeSwitching(world, input);

    // Motion from the previous mode must not rotate the new camera pose.
    if (!modeChanged) {
        updateCamera(world, input, frameSeconds, alpha);
    }

    updateFlashlight(world, alpha);
    updateMoon(world);
}

Camera& DemoGame::getCamera() {
    return camera;
}

bool DemoGame::wantsMouseLook() const {
    return controlMode != ControlMode::Moba;
}

Entity DemoGame::getPlayer() const {
    return player;
}

Entity DemoGame::getDestinationMarker() const {
    return destinationMarker;
}

Entity DemoGame::getFlashlight() const {
    return flashlight;
}

Entity DemoGame::getMoon() const {
    return moon;
}

void DemoGame::setControlMode(World& world, ControlMode mode) {
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};

    // Each selection starts from a predictable test position.
    switch (mode) {
    case ControlMode::FirstPerson:
        camera.setPose(
            Vec3{0.0f, 1.0f, 0.0f},
            Vec3{0.0f, 1.0f, -6.0f},
            worldUp
        );
        firstPersonCamera.takeOver(camera);
        break;

    case ControlMode::Moba:
        mobaCamera.centreOn(camera, Vec3{0.0f, 0.0f, -6.0f});
        break;

    case ControlMode::FreeCamera:
        camera.setPose(
            Vec3{2.0f, 1.0f, 0.0f},
            Vec3{0.0f, 0.0f, -5.0f},
            worldUp
        );
        freeFlyCamera.takeOver(camera);
        break;

    default:
        throw std::invalid_argument("Unknown control mode");
    }

    controlMode = mode;
    groundSteeringActive = false;
    if (world.isAlive(player)) {
        world.get<CharacterMovement>(player).stop();

        // From the MOBA camera's height the duck is small, so it glows to stay easy to find at night. Emitted light does not
        // light its surroundings. In the other modes it is lit like everything else.
        const Vec3 glow = mode == ControlMode::Moba ? Vec3{0.45f, 0.35f, 0.12f} : Vec3{0.0f, 0.0f, 0.0f};

        for (RenderPart& part : world.get<ModelRenderer>(player).parts) {
            part.material.emissive = glow;
        }
    }
    world.get<ModelRenderer>(destinationMarker).visible = false;
}

bool DemoGame::updateModeSwitching(World& world, const Input& input) {
    if (!enableDebugModeSwitching || !input.hasKeyboardFocus()) {
        return false;
    }

    if (input.wasKeyPressed(Key::F1)) {
        setControlMode(world, ControlMode::FirstPerson);
        return true;
    }

    if (input.wasKeyPressed(Key::F2)) {
        setControlMode(world, ControlMode::Moba);
        return true;
    }

    if (input.wasKeyPressed(Key::F3)) {
        setControlMode(world, ControlMode::FreeCamera);
        return true;
    }

    return false;
}

/*
* Each mode hands the camera to its own controller. Toggles such as the MOBA lock are the demo's rules, so they stay here.
*/
void DemoGame::updateCamera(const World& world, const Input& input, float frameSeconds, float alpha) {
    switch (controlMode) {
    case ControlMode::FirstPerson:
        firstPersonCamera.update(camera, input, frameSeconds);
        break;

    case ControlMode::FreeCamera:
        freeFlyCamera.update(camera, input, frameSeconds);
        break;

    case ControlMode::Moba: {
        if (input.isCursorConfined() && input.wasKeyPressed(Key::Space)) {
            mobaCameraLocked = !mobaCameraLocked;
        }

        // Follow where the character is drawn, not where the latest tick put it, so the camera and the character move together smoothly.
        std::optional<Vec3> followTarget;
        if (mobaCameraLocked && world.isAlive(player)) {
            followTarget = getCharacterVisualCentre(world, player, alpha);
        }

        mobaCamera.update(camera, input, frameSeconds, followTarget);
        break;
    }
    }
}

/*
* Turn right-clicks into move commands. A click uses the exact position from its event; holding the button keeps steering towards the live cursor.
*/
void DemoGame::updatePlayerCommands(World& world, const Input& input) {
    // Only accept new commands while MOBA input is active.
    const bool commandsActive =
        world.isAlive(player) &&
        controlMode == ControlMode::Moba &&
        input.hasKeyboardFocus() &&
        input.isCursorConfined();

    if (!commandsActive) {
        groundSteeringActive = false;
        return;
    }

    std::optional<Vec2> target;

    if (const auto press = input.getLastMouseButtonPress(MouseButton::Right)) {
        target = press->position;

        // A tap that was pressed and released within one frame moves once but does not start steering.
        groundSteeringActive = input.isMouseButtonHeld(MouseButton::Right);
    } else if (groundSteeringActive) {
        if (input.isMouseButtonHeld(MouseButton::Right) && input.isCursorInWindow()) {
            target = input.getCursorPosition();
        } else {
            groundSteeringActive = false;
        }
    }

    if (!target) {
        return;
    }

    const Vec2 size = input.getWindowSize();

    if (size.x <= 0.0f || size.y <= 0.0f ||
        target->x < 0.0f || target->y < 0.0f ||
        target->x >= size.x || target->y >= size.y) {
        return;
    }

    const Ray ray = makeCameraRay(
        camera,
        target->x / size.x,
        target->y / size.y,
        size.x / size.y
    );

    const auto hit = intersectGround(ray, 0.0f);

    if (!hit) {
        return;
    }

    const bool insideGround =
        hit->x >= -20.0f &&
        hit->x <= 20.0f &&
        hit->z >= -26.0f &&
        hit->z <= 14.0f;

    if (!insideGround) {
        return;
    }

    world.get<CharacterMovement>(player).moveTo(*hit);

    // The marker has no PreviousTransform, so it jumps straight to the new spot.
    world.get<Transform>(destinationMarker).position = Vec3{
        hit->x,
        0.02f,
        hit->z
    };
    world.get<ModelRenderer>(destinationMarker).visible = true;
}
