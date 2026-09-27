#include "application.h"
#include "mat4.h"
#include "vec3.h"
#include "obj_loader.h"
#include "gltf_loader.h"
#include "ray.h"

#include <SDL3/SDL.h>
#include <numbers>
#include <memory>
#include <cmath>
#include <stdexcept>

namespace {
    float animatedAngle(
        float initialAngle,
        float speed,
        double elapsedSeconds
    ) {
        const double angle =
            static_cast<double>(initialAngle) +
            static_cast<double>(speed) * elapsedSeconds;

        const double fullTurn = 2.0 * std::numbers::pi_v<double>;

        return static_cast<float>(std::fmod(angle, fullTurn));
    }
}

Application::Application(int width, int height):
    display("My Engine", width, height),
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
    createScene();
    uploadSceneMeshes();
}

void Application::createScene(){
    const std::shared_ptr<const Mesh> cubeMesh = std::make_shared<Mesh>(Mesh::cube());
    const std::shared_ptr<const Mesh> pyramidMesh = std::make_shared<Mesh>(loadObj("assets/models/pyramid.obj"));
    const std::shared_ptr<const Mesh> teapotMesh = std::make_shared<Mesh>(loadObj("assets/models/teapot.obj"));

    MeshInstance first{cubeMesh};
    first.transform.position  = Vec3{-3.0f,  0.0f, -10.0f};
    first.transform.rotation.x = 0.3f;
    first.material.colour = Pixel{255, 255, 255};

    MeshInstance second{cubeMesh};
    second.transform.position = Vec3{ 3.0f,  0.0f, -10.0f};
    second.transform.scale = Vec3{0.7f, 0.7f, 0.7f};
    second.material.colour = Pixel{255, 255, 255};

    first.initialRotation = first.transform.rotation;
    first.rotationSpeed = Vec3{2.0f, 2.0f, 0.0f};

    second.initialRotation = second.transform.rotation;
    second.rotationSpeed = Vec3{0.0f, -1.0f, 0.0f};

    MeshInstance third{pyramidMesh};
    third.transform.position  = Vec3{ 0.0f,  3.0f, -10.0f};
    third.transform.scale = Vec3{0.7f, 0.7f, 0.7f};
    third.material.colour = Pixel{80, 200, 120};

    third.initialRotation = third.transform.rotation;
    third.rotationSpeed = Vec3{0.0f, 1.0f, 0.0f};

    MeshInstance fourth{teapotMesh};
    fourth.transform.position = Vec3{ 0.0f, -3.0f, -10.0f};
    fourth.transform.scale = Vec3{0.7f, 0.7f, 0.7f};
    fourth.material.colour = Pixel{230, 180, 60};

    fourth.initialRotation = fourth.transform.rotation;
    fourth.rotationSpeed = Vec3{0.0f, -1.0f, 0.0f};

    first.material.texturePath = "assets/textures/demo.png";
    second.material.texturePath = "assets/textures/demo.png";

    scene.add(first);
    scene.add(second);
    scene.add(third);
    scene.add(fourth);

    // Ground
    const auto groundMesh =
        std::make_shared<Mesh>(Mesh::plane(20.0f));

    MeshInstance ground{groundMesh};

    ground.transform.position = Vec3{0.0f, 0.0f, -6.0f};
    ground.material.colour = Pixel{75, 110, 75};

    scene.add(ground);

    // Duck
    const Model model = loadGltf("assets/models/Duck.glb");

    const Mat4 normalization =
        model.getNormalizationMatrix(2.0f);

    const ModelBounds bounds = model.getBounds();

    // Normalization centres the model, so its bottom lies below zero.
    const Vec4 normalizedBottom = normalization * Vec4{
        bounds.minimum.x,
        bounds.minimum.y,
        bounds.minimum.z,
        1.0f
    };

    Transform placement;
    placement.position = Vec3{
        0.0f,
        -normalizedBottom.y,
        -6.0f
    };

    duck = scene.addModel(
        model,
        placement,
        normalization,
        Vec3{0.0f, 0.0f, 0.0f}
    );
}

/*
* Run the main loop continuously process input, update the scene, and render each frame.
*/
void Application::run() {
    Uint64 previousFrameStart = SDL_GetTicksNS();

    while (true) {
        const Uint64 frameStart = SDL_GetTicksNS();

        if (!display.processEvents()) {
            break;
        }

        const double deltaSeconds =
            static_cast<double>(frameStart - previousFrameStart) /
            1'000'000'000.0;

        previousFrameStart = frameStart;
        elapsedSeconds += deltaSeconds;

        update(static_cast<float>(deltaSeconds));
        render();

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
* Update the camera position based on user input and the frame time delta to provide consistent movement speed regardless of frame rate.
*/
void Application::update(float deltaTime) {
    // Pick using the camera pose before this frame's panning.
    updateDuckMovement(deltaTime);

    updateCameraControls(deltaTime);

    // Follow the duck's updated position.
    followDuckWithCamera();

    const auto animateRotation = [this](Transform& transform, const Vec3& initial, const Vec3& speed) {
        // Leave manually controlled axes alone when their speed is zero.
        if (speed.x != 0.0f) {
            transform.rotation.x = animatedAngle(
                initial.x, speed.x, elapsedSeconds
            );
        }

        if (speed.y != 0.0f) {
            transform.rotation.y = animatedAngle(
                initial.y, speed.y, elapsedSeconds
            );
        }

        if (speed.z != 0.0f) {
            transform.rotation.z = animatedAngle(
                initial.z, speed.z, elapsedSeconds
            );
        }
    };

    // Standalone meshes, such as the original cubes.
    for (MeshInstance& object : scene.getObjects()) {
        if (!object.modelInstance) {
            animateRotation(
                object.transform,
                object.initialRotation,
                object.rotationSpeed
            );
        }
    }

    // Imported models: update their shared transform once.
    for (const auto& instance : scene.getModelInstances()) {
        animateRotation(
            instance->transform,
            instance->initialRotation,
            instance->rotationSpeed
        );
    }
}

void Application::uploadSceneMeshes() {
    for (const MeshInstance& object : scene.getObjects()) {
        if (!gpuMeshes.contains(object.mesh)) {
            gpuMeshes.emplace(
                object.mesh,
                std::make_unique<GpuMesh>(
                    display.getDevice(),
                    *object.mesh
                )
            );
        }
    }
}

void Application::render() {
    // Upload any newly requested textures before starting the frame.
    // Already-cached textures require only a lookup.
    for (const MeshInstance& object : scene.getObjects()) {
        display.prepareMaterial(object.material);
    }

    if (!display.beginFrame(0.0f, 0.0f, 0.0f)) {
        return;
    }

    // Use the actual dimensions of the frame acquired by beginFrame().
    camera.setAspectRatio(display.getFrameAspectRatio());

    // Convert our projection's depth range to the GPU depth range.
    Mat4 depthCorrection = Mat4::identity();
    depthCorrection.values[2][2] = 0.5f;
    depthCorrection.values[2][3] = 0.5f;

    const Mat4 viewProjection =
        depthCorrection *
        camera.getProjectionMatrix() *
        camera.getViewMatrix();

    for (const MeshInstance& object : scene.getObjects()) {
        const GpuMesh& gpuMesh = *gpuMeshes.at(object.mesh);
        const Mat4 model = object.getModelMatrix();

        display.drawMesh(
            gpuMesh,
            model,
            viewProjection,
            object.material
        );
    }

    display.endFrame();
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
    duckMovement.stop();

    display.setMouseLookEnabled(
        mode != ControlMode::Moba
    );
}

void Application::setDebugModeSwitching(bool enabled) {
    enableDebugModeSwitching = enabled;
}

void Application::updateCameraControls(float deltaTime) {
    if (!display.hasKeyboardFocus()) {
        return;
    }

    if (enableDebugModeSwitching) {
        if (display.wasKeyPressed(SDL_SCANCODE_F1)) {
            setControlMode(ControlMode::FirstPerson);
        } else if (display.wasKeyPressed(SDL_SCANCODE_F2)) {
            setControlMode(ControlMode::Moba);
        } else if (display.wasKeyPressed(SDL_SCANCODE_F3)) {
            setControlMode(ControlMode::FreeCamera);
        }
    }

    if (controlMode == ControlMode::Moba) {
        updateMobaCamera(deltaTime);
        return;
    }

    if (controlMode != ControlMode::Moba) {
        // Escape pauses mouse-look controls until the next click.
        if (!display.isMouseCaptured()) {
            return;
        }

        const Vec2 mouseDelta = display.getMouseDelta();

        cameraController.look(
            camera,
            mouseDelta.x,
            mouseDelta.y
        );
    }

    cameraController.update(
        camera,
        deltaTime,
        controlMode
    );
}

void Application::updateMobaCamera(float deltaTime) {
    if (display.isCursorConfined() &&
        display.wasKeyPressed(SDL_SCANCODE_SPACE)) {
        mobaCameraLocked = !mobaCameraLocked;
    }

    if (mobaCameraLocked) {
        return;
    }

    const Vec2 edge = display.getEdgePanDirection();

    // Screen left/right maps to world X.
    // Screen top/bottom maps to world -Z/+Z.
    const Vec3 movement{edge.x, 0.0f, edge.y};

    if (movement.lengthSquared() > 0.0f) {
        camera.move(
            movement.normalized() * (mobaPanSpeed * deltaTime)
        );
    }
}

void Application::updateDuckMovement(float deltaTime) {
    if (!duck) {
        return;
    }

    // Only accept new commands while MOBA input is active.
    if (controlMode == ControlMode::Moba &&
        display.hasKeyboardFocus() &&
        display.isCursorConfined()) {
        const auto click = display.getGroundClick();

        if (click) {
            const Ray ray = makeCameraRay(
                camera,
                click->x,
                click->y,
                click->aspectRatio
            );

            const auto hit = intersectGround(ray, 0.0f);

            if (hit) {
                const bool insideGround =
                    hit->x >= -20.0f &&
                    hit->x <= 20.0f &&
                    hit->z >= -26.0f &&
                    hit->z <= 14.0f;

                if (insideGround) {
                    duckMovement.setTarget(*hit);
                }
            }
        }
    }

    // Continue an existing command regardless of cursor or focus.
    const Vec3 previousPosition = duck->transform.position;

    duckMovement.update(
        duck->transform.position,
        deltaTime
    );

    const Vec3 movement =
        duck->transform.position - previousPosition;

    if (movement.x != 0.0f || movement.z != 0.0f) {
        // Heading measured from world +Z toward world +X.
        const float heading = std::atan2(movement.x, movement.z);

        // Adjust this if the imported model faces a different direction.
        constexpr float modelForwardYaw = std::numbers::pi_v<float> / 2.0f;

        duck->transform.rotation.y = heading - modelForwardYaw;
    }
}

void Application::followDuckWithCamera() {
    if (controlMode == ControlMode::Moba && mobaCameraLocked && duck) {
        const Vec3 target = duck->transform.position;

        camera.setPose(
            target + mobaCameraOffset,
            target,
            Vec3{0.0f, 1.0f, 0.0f}
        );
    }
}