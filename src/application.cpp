#include "application.h"
#include "mat4.h"
#include "vec3.h"
#include "obj_loader.h"

#include <SDL3/SDL.h>
#include <numbers>
#include <memory>
#include <cmath>

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
    if (display.isMouseCaptured()) {
        const Vec2 mouseDelta = display.getMouseDelta();

        cameraController.look(camera, mouseDelta.x, mouseDelta.y);

        cameraController.update(camera, deltaTime);
    }

    for (MeshInstance& object : scene.getObjects()) {
        object.transform.rotation = Vec3{
            animatedAngle(
                object.initialRotation.x,
                object.rotationSpeed.x,
                elapsedSeconds
            ),
            animatedAngle(
                object.initialRotation.y,
                object.rotationSpeed.y,
                elapsedSeconds
            ),
            animatedAngle(
                object.initialRotation.z,
                object.rotationSpeed.z,
                elapsedSeconds
            )
        };
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
        const Mat4 model = object.transform.getMatrix();

        display.drawMesh(
            gpuMesh,
            model,
            viewProjection,
            object.material
        );
    }

    display.endFrame();
}