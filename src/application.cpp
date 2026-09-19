#include "application.h"
#include "mat4.h"
#include "vec3.h"

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
    buffer(width, height),
    display("My Engine", width, height),
    renderer(buffer),
    camera(
        Vec3{2.0f, 1.0f, 0.0f},
        Vec3{0.0f, 0.0f, -5.0f},
        Vec3{0.0f, 1.0f, 0.0f},
        std::numbers::pi_v<float> / 2.0f,
        static_cast<float>(buffer.getWidth()) / static_cast<float>(buffer.getHeight()),
        0.1f,
        100.0f
    ),
    cameraController(3.0f)
{
    createScene();
}

void Application::createScene(){
    const std::shared_ptr<const Mesh> cubeMesh = std::make_shared<Mesh>(Mesh::cube());

    MeshInstance first{cubeMesh};
    first.transform.position = Vec3{-2.0f, 0.0f, -7.0f};
    first.transform.rotation.x = 0.3f;
    first.colour = Pixel{220, 80, 80};

    MeshInstance second{cubeMesh};
    second.transform.position = Vec3{2.0f, 0.0f, -7.0f};
    second.transform.scale = Vec3{0.7f, 0.7f, 0.7f};
    second.colour = Pixel{80, 120, 220};

    first.initialRotation = first.transform.rotation;
    first.rotationSpeed = Vec3{2.0f, 2.0f, 0.0f};

    second.initialRotation = second.transform.rotation;
    second.rotationSpeed = Vec3{0.0f, -1.0f, 0.0f};

    scene.add(first);
    scene.add(second);
}

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

void Application::update(float deltaTime) {
    cameraController.update(camera, deltaTime);

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

void Application::render() {
    renderer.clear(Pixel{0, 0, 0});

    for (const MeshInstance& object : scene.getObjects()) {
        renderer.drawMesh(
            *object.mesh,
            object.transform.getMatrix(),
            camera,
            object.colour,
            false
        );
    }

    display.present(buffer);
}