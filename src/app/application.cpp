#include "app/application.h"
#include "math/mat4.h"
#include "math/vec3.h"
#include "assets/obj_loader.h"
#include "assets/gltf_loader.h"
#include "scene/camera_ray.h"
#include "scene/interpolation.h"
#include "scene/model_renderer.h"
#include "gameplay/spinner.h"
#include "gameplay/edge_pan.h"

#include <SDL3/SDL.h>
#include <numbers>
#include <memory>
#include <optional>
#include <stdexcept>

Application::Application(int width, int height):
    window("My Engine", width, height),
    renderer(window.getSdlWindow()),
    camera(
        Vec3{2.0f, 1.0f, 0.0f},
        Vec3{0.0f, 0.0f, -5.0f},
        Vec3{0.0f, 1.0f, 0.0f},
        70.0f * std::numbers::pi_v<float> / 180.0f, // FoV
        static_cast<float>(width) / static_cast<float>(height),
        0.1f,
        100.0f
    ),
    // Movement speed, mouse sensitivity.
    cameraController(3.0f, 0.001f)
{
    // Low latency without tearing. Falls back to vsync on GPUs without mailbox support.
    renderer.setPresentMode(PresentMode::Mailbox);

    createScene();
    uploadSceneMeshes();
}

void Application::createScene(){
    const std::shared_ptr<const Mesh> cubeMesh = std::make_shared<Mesh>(Mesh::cube());
    const std::shared_ptr<const Mesh> pyramidMesh = std::make_shared<Mesh>(loadObj("assets/models/pyramid.obj"));
    const std::shared_ptr<const Mesh> teapotMesh = std::make_shared<Mesh>(loadObj("assets/models/teapot.obj"));

    // A spinning demo object: Transform + ModelRenderer + Spinner, plus PreviousTransform so it is drawn smoothly.
    const auto spawnSpinner = [this](std::shared_ptr<const Mesh> mesh, const Material& material,
                                     const Transform& transform, const Vec3& speed) {
        const Entity entity = world.create();
        world.add(entity, transform);
        world.add(entity, PreviousTransform{transform});
        world.add(entity, makeMeshRenderer(std::move(mesh), material));
        world.add(entity, Spinner{transform.rotation, speed});
    };

    Material textured;
    textured.texturePath = "assets/textures/demo.png";

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
    fourth.position = Vec3{0.0f, -3.0f, -10.0f};
    fourth.scale = smallScale;
    spawnSpinner(teapotMesh, gold, fourth, Vec3{0.0f, -1.0f, 0.0f});

    // Ground: it never moves, so it needs no PreviousTransform.
    Material grass;
    grass.colour = Pixel{75, 110, 75};

    Transform groundPlacement;
    groundPlacement.position = Vec3{0.0f, 0.0f, -6.0f};

    const Entity ground = world.create();
    world.add(ground, groundPlacement);
    world.add(ground, makeMeshRenderer(std::make_shared<Mesh>(Mesh::plane(20.0f)), grass));

    // Change this configuration to use another compatible static model.
    const CharacterConfig playerConfig{
        .modelPath = "assets/models/Duck.glb",
        .modelSize = 2.0f,
        .movementSpeed = 6.0f,
        .turnSpeed = 6.0f * std::numbers::pi_v<float>,
        .modelForwardYaw = std::numbers::pi_v<float> / 2.0f
    };

    const Model model = loadGltf(playerConfig.modelPath);
    player = spawnCharacter(world, model, playerConfig, Vec3{0.0f, 0.0f, -6.0f});

    // Movement marker. It jumps to each click instead of gliding, so it has no PreviousTransform.
    Material yellow;
    yellow.colour = Pixel{255, 220, 40};

    ModelRenderer markerRenderer = makeMeshRenderer(std::make_shared<Mesh>(Mesh::plane(0.2f)), yellow);
    markerRenderer.visible = false;

    // Slightly above the ground to avoid overlapping surfaces.
    Transform markerPlacement;
    markerPlacement.position = Vec3{0.0f, 0.02f, 0.0f};

    destinationMarker = world.create();
    world.add(destinationMarker, markerPlacement);
    world.add(destinationMarker, std::move(markerRenderer));
}

/*
* Run the main loop. Each frame reads input once, runs zero or more fixed simulation ticks, then updates the camera and renders between the last two ticks.
*
* Input is only read here, once per frame, never inside a tick. Clicks become commands (such as moveTo) that the next tick picks up, so a frame that runs zero ticks cannot lose a press and a frame that runs two cannot apply it twice.
*/
void Application::run() {
    Uint64 previousFrameStart = SDL_GetTicksNS();

    while (true) {
        const Uint64 frameStart = SDL_GetTicksNS();

        if (!window.processEvents(input)) {
            break;
        }

        const double deltaSeconds =
            static_cast<double>(frameStart - previousFrameStart) /
            1'000'000'000.0;

        previousFrameStart = frameStart;

        const int ticks = timestep.advance(deltaSeconds);

        // Pick using the camera pose before this frame's panning.
        updatePlayerCommands();

        for (int tick = 0; tick < ticks; ++tick) {
            simulate(static_cast<float>(timestep.getTickSeconds()));
        }

        // The camera is the player's view, not game state, so it updates every rendered frame.
        // Mouse look in particular must not be limited to the tick rate.
        const float alpha = timestep.getAlpha();
        updateCameraControls(static_cast<float>(timestep.getFrameSeconds()));
        followPlayerWithCamera(alpha);

        render(alpha);

        // 0 means unlimited.
        if (targetFPS > 0) {
            const Uint64 frameDuration =
                1'000'000'000ULL / targetFPS;

            const Uint64 elapsed =
                SDL_GetTicksNS() - frameStart;

            if (elapsed < frameDuration) {
                SDL_DelayNS(frameDuration - elapsed);
            }
        }
    }
}

/*
* Advance the game state by one fixed tick. Everything in here sees the same tickSeconds every time, so results do not depend on the frame rate.
*/
void Application::simulate(float tickSeconds) {
    ++simulationTicks;
    const double simulationSeconds =
        static_cast<double>(simulationTicks) * timestep.getTickSeconds();

    // The systems, in a fixed order.
    // Rendering blends from the transforms saved here to the ones this tick produces.
    savePreviousTransforms(world);
    updateCharacters(world, tickSeconds);
    updateSpinners(world, simulationSeconds);

    // Simulation continues when the cursor is released or focus is lost.
    if (world.isAlive(player) && !world.get<CharacterMovement>(player).isMoving()) {
        world.get<ModelRenderer>(destinationMarker).visible = false;
    }
}

void Application::uploadSceneMeshes() {
    world.each<ModelRenderer>([this](Entity, ModelRenderer& modelRenderer) {
        for (const RenderPart& part : modelRenderer.parts) {
            if (!gpuMeshes.contains(part.mesh)) {
                gpuMeshes.emplace(
                    part.mesh,
                    std::make_unique<GpuMesh>(renderer.getDevice(), *part.mesh)
                );
            }
        }
    });
}

/*
* Draw the scene. alpha (0..1) is how far real time has moved past the last tick, and each object is drawn that far between its previous and current transform.
*/
void Application::render(float alpha) {
    // Upload any newly requested textures before starting the frame.
    // Already-cached textures require only a lookup.
    world.each<ModelRenderer>([this](Entity, ModelRenderer& modelRenderer) {
        for (const RenderPart& part : modelRenderer.parts) {
            renderer.prepareMaterial(part.material);
        }
    });

    if (!renderer.beginFrame(0.0f, 0.0f, 0.0f)) {
        return;
    }

    // Use the actual dimensions of the frame acquired by beginFrame().
    camera.setAspectRatio(renderer.getFrameAspectRatio());

    // Convert our projection's depth range to the GPU depth range.
    Mat4 depthCorrection = Mat4::identity();
    depthCorrection.values[2][2] = 0.5f;
    depthCorrection.values[2][3] = 0.5f;

    const Mat4 viewProjection =
        depthCorrection *
        camera.getProjectionMatrix() *
        camera.getViewMatrix();

    // Draw every entity that has something to draw and a place to draw it.
    world.each<ModelRenderer, Transform>([&](Entity entity, ModelRenderer& modelRenderer, Transform&) {
        if (!modelRenderer.visible) {
            return;
        }

        const Mat4 entityMatrix = getRenderTransform(world, entity, alpha).getMatrix();

        for (const RenderPart& part : modelRenderer.parts) {
            renderer.drawMesh(
                *gpuMeshes.at(part.mesh),
                entityMatrix * part.localTransform,
                viewProjection,
                part.material
            );
        }
    });

    renderer.endFrame();
}

void Application::setControlMode(ControlMode mode) {
    const Vec3 worldUp{0.0f, 1.0f, 0.0f};

    // Each selection starts from a predictable test position.
    switch (mode) {
    case ControlMode::FirstPerson:
        camera.setPose(
            Vec3{0.0f, 1.0f, 0.0f},
            Vec3{0.0f, 1.0f, -6.0f},
            worldUp
        );
        break;

    case ControlMode::Moba:
        camera.setPose(
            Vec3{0.0f, 12.0f, 4.0f},
            Vec3{0.0f, 0.0f, -6.0f},
            worldUp
        );
        break;

    case ControlMode::FreeCamera:
        camera.setPose(
            Vec3{2.0f, 1.0f, 0.0f},
            Vec3{0.0f, 0.0f, -5.0f},
            worldUp
        );
        break;

    default:
        throw std::invalid_argument("Unknown control mode");
    }

    controlMode = mode;
    groundSteeringActive = false;
    if (world.isAlive(player)) {
        world.get<CharacterMovement>(player).stop();
    }
    world.get<ModelRenderer>(destinationMarker).visible = false;

    window.setMouseLookEnabled(
        mode != ControlMode::Moba
    );

    // Motion from the previous mode must not rotate the new camera pose.
    input.discardMouseMotion();
}

void Application::setDebugModeSwitching(bool enabled) {
    enableDebugModeSwitching = enabled;
}

void Application::updateCameraControls(float deltaTime) {
    if (!window.hasKeyboardFocus()) {
        return;
    }

    if (enableDebugModeSwitching) {
        if (input.wasKeyPressed(Key::F1)) {
            setControlMode(ControlMode::FirstPerson);
        } else if (input.wasKeyPressed(Key::F2)) {
            setControlMode(ControlMode::Moba);
        } else if (input.wasKeyPressed(Key::F3)) {
            setControlMode(ControlMode::FreeCamera);
        }
    }

    if (controlMode == ControlMode::Moba) {
        updateMobaCamera(deltaTime);
        return;
    }

    if (controlMode != ControlMode::Moba) {
        // Escape pauses mouse-look controls until the next click.
        if (!window.isMouseCaptured()) {
            return;
        }

        const Vec2 mouseDelta = input.getMouseDelta();

        cameraController.look(
            camera,
            mouseDelta.x,
            mouseDelta.y
        );
    }

    cameraController.update(
        camera,
        input,
        deltaTime,
        controlMode
    );
}

void Application::updateMobaCamera(float deltaTime) {
    if (window.isCursorConfined() &&
        input.wasKeyPressed(Key::Space)) {
        mobaCameraLocked = !mobaCameraLocked;
    }

    if (mobaCameraLocked) {
        return;
    }

    // Only pan while the MOBA cursor is confined to a focused window, so moving to another monitor does not scroll the map.
    const bool panningActive = window.isCursorConfined() && window.hasKeyboardFocus();
    const Vec2 edge = panningActive ? getEdgePanDirection(input) : Vec2{};

    // Screen left/right maps to world X.
    // Screen top/bottom maps to world -Z/+Z.
    const Vec3 movement{edge.x, 0.0f, edge.y};

    if (movement.lengthSquared() > 0.0f) {
        camera.move(
            movement.normalized() * (mobaPanSpeed * deltaTime)
        );
    }
}

/*
* Turn right-clicks into move commands. A click uses the exact position from its event; holding the button keeps steering towards the live cursor.
*/
void Application::updatePlayerCommands() {
    // Only accept new commands while MOBA input is active.
    const bool commandsActive =
        world.isAlive(player) &&
        controlMode == ControlMode::Moba &&
        window.hasKeyboardFocus() &&
        window.isCursorConfined();

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

/*
* Follow where the character is drawn, not where the latest tick put it, so the camera and the character move together smoothly.
*/
void Application::followPlayerWithCamera(float alpha) {
    if (controlMode == ControlMode::Moba && mobaCameraLocked && world.isAlive(player)) {
        const Vec3 target = getCharacterVisualCentre(world, player, alpha);

        camera.setPose(
            target + mobaCameraOffset,
            target,
            Vec3{0.0f, 1.0f, 0.0f}
        );
    }
}
