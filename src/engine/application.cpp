#include "engine/application.h"
#include "engine/debug_views.h"
#include "scene/culling.h"
#include "scene/frame_description.h"
#include "scene/indexed_mesh.h"

#include <SDL3/SDL.h>
#include <cstdint>
#include <span>

Application::Application(const char* title, int width, int height, Game& game):
    window(title, width, height),
    renderer(window.getSdlWindow()),
    game(game)
{
    // Low latency without tearing. Falls back to vsync on GPUs without mailbox support.
    renderer.setPresentMode(PresentMode::Mailbox);

    game.onInit(world, assets);

    mouseLookEnabled = game.wantsMouseLook();
    window.setMouseLookEnabled(mouseLookEnabled);
}

/*
* Run the main loop. Each frame reads input once, lets the game turn it into commands, runs zero or more fixed simulation ticks, then lets the game move the camera and draws between the last two ticks.
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

        game.onInput(world, input);

        for (int tick = 0; tick < ticks; ++tick) {
            ++simulationTicks;
            const double simulationSeconds =
                static_cast<double>(simulationTicks) * timestep.getTickSeconds();

            runFixedUpdate(game, world, static_cast<float>(timestep.getTickSeconds()), simulationSeconds);
        }

        const float alpha = timestep.getAlpha();
        game.onUpdate(world, input, static_cast<float>(timestep.getFrameSeconds()), alpha);

        applyMouseMode();
        render(alpha, static_cast<float>(timestep.getFrameSeconds()));

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

void Application::applyMouseMode() {
    const bool wanted = game.wantsMouseLook();

    if (wanted != mouseLookEnabled) {
        mouseLookEnabled = wanted;
        window.setMouseLookEnabled(wanted);
    }
}

/*
* Games may load assets at any time. Assets are only appended, so the new ones are exactly those past the renderer's count: no per-entity checks.
* Uploads must happen before the frame begins.
*/
void Application::prepareNewResources() {
    while (renderer.getMeshCount() < assets.getMeshCount()) {
        const MeshHandle next{static_cast<std::uint32_t>(renderer.getMeshCount())};
        // Convert to the render format once, when the mesh first reaches the GPU.
        renderer.uploadMesh(buildIndexedMesh(assets.getMesh(next)));
        boundsByMesh.push_back(meshBounds(assets.getMesh(next)));
    }

    while (renderer.getTextureCount() < assets.getTextureCount()) {
        const TextureHandle next{static_cast<std::uint32_t>(renderer.getTextureCount())};
        const ImageData& image = assets.getTexture(next);
        renderer.uploadTexture(image.width, image.height, std::span<const Uint8>{image.pixels.data(), image.pixels.size()});
    }
}

/*
* Draw the scene. alpha (0..1) is how far real time has moved past the last tick, and each object is drawn that far between its previous and current transform.
*/
void Application::render(float alpha, float frameSeconds) {
    prepareNewResources();

    Camera& camera = game.getCamera();
    FrameDescription frame = buildFrame(world, camera, alpha);
    frame.lightFocus = game.getLightFocus();
    frame.frameSeconds = frameSeconds;

    // Spheres around each draw let the renderer skip what a view cannot see.
    attachBoundingSpheres(frame.draws, boundsByMesh);

    // Debug shapes are gathered fresh every frame, so anything not added again disappears.
    debugDraw.clear();

    if (game.wantsDebugView()) {
        drawBoundingBoxes(frame.draws, boundsByMesh, debugDraw);

        // From the previous frame: this one has not been drawn yet. At hundreds of frames a second the lag is invisible.
        drawRenderStats(renderer.getLastFrameStats(), debugDraw);
    }

    game.onDebugDraw(world, debugDraw, alpha);
    frame.debugLines = debugDraw.getLines();
    frame.debugScreenLines = debugDraw.getScreenLines();

    if (!renderer.render(frame)) {
        return;
    }

    // The renderer fitted its copy of the camera to the frame. The game's camera gets the same aspect ratio,
    // so turning clicks into rays (makeCameraRay) matches what is on screen.
    camera.setAspectRatio(renderer.getFrameAspectRatio());
}
