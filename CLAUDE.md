# GameEngine

A from-scratch C++20 game engine. Long-term goal: support both **FPS** and **MOBA** style games by keeping the core genre-agnostic and putting genre-specific code (camera controllers, weapons, abilities, netcode style) in separate modules on top.

The roadmap is in `external/plan.png`. Steps 1-6 are done, step 7 (shading/lighting) is partial, steps 8-10 (particle physics, rigid bodies, engine architecture) are not started.

## Build and run

- `make` builds `build/game.exe` and compiles shaders (`glslc` -> `.spv`).
- `make run` builds and runs the game. Run it from the repo root: asset and shader paths are relative (`assets/...`).
- `make test` builds and runs `build/tests.exe`.
- The makefile uses wildcards for `src/*.cpp` and `tests/*.cpp`, so new files are picked up automatically. Headers and the makefile are tracked as dependencies.
- Flags are `-std=c++20 -Wall -Wextra -g` with no optimisation (-O0). A release target with `-O2` is a planned improvement.
- Dependencies: SDL3, SDL3_image, `glslc` (Vulkan SDK), and cgltf (vendored in `external/cgltf`).
- `build/`, `*.exe` and `*.spv` are git-ignored.
- Source files use CRLF line endings. Preserve them when editing.

## Architecture (current)

Live path: `main` -> `Application` -> `GpuDisplay`.

- `Application` runs the main loop (fixed timestep, frame cap), owns the `Input`, `Camera`, `CameraController`, `Scene`, the player `Character`, and a map from shared `Mesh` to uploaded `GpuMesh`. `display` is declared first so the GPU device outlives all GPU resources.
- `GpuDisplay` uses SDL3's GPU API (SDL_GPU) with the Vulkan backend and SPIR-V shaders. It owns the window, device, pipeline, depth texture, texture cache, event polling, mouse capture, cursor confinement, MOBA edge-pan and right-click ground clicks (`GroundClick`, normalized window coordinates), and fullscreen (Alt+Enter).
- `Input` is a once-per-frame snapshot built from SDL events: `isKeyHeld`, `wasKeyPressed`, `wasKeyReleased`, and accumulated `getMouseDelta`. `GpuDisplay::processEvents(input)` resets and fills it; window-level events (quit, focus, capture, Escape, Alt+Enter) stay in `GpuDisplay`. Mouse motion is only forwarded while the mouse is captured, and is discarded when capture or control mode changes. Game code reads keys from `Input`, never from `SDL_GetKeyboardState`.
- Main loop: `FixedTimestep` (120 Hz, frame time clamped to 0.25 s) turns real frame time into a whole number of ticks plus an interpolation `alpha`. Each frame: read input and turn clicks into commands (`updatePlayerCommands`), run `simulate(tickSeconds)` zero or more times, update the camera per frame (`updateCameraControls`, `followPlayerWithCamera(alpha)`), then `render(alpha)`. Never read `Input` inside `simulate`; key/click edges are handled once per frame and reach the simulation as commands. Mouse look and camera movement are per frame, not per tick.
- Interpolation: `MeshInstance` and `ModelInstance` hold `previousTransform`, saved by `Scene::savePreviousTransforms()` at the start of each tick. Rendering uses `getInterpolatedModelMatrix(alpha)`; `interpolate(Transform, Transform, alpha)` takes the shortest way around for angles. `Scene::add`/`addModel` start objects snapped (previous = current); anything teleported outside a tick must also snap, or it visibly slides (see the destination marker).
- Control modes (`ControlMode`): F1 first-person, F2 MOBA, F3 free camera (debug switching, `setDebugModeSwitching`). MOBA mode uses a confined visible cursor, edge panning, Space to lock the camera on the player, and right-click (or hold) to move the player.
- `Character` (configured by `CharacterConfig`) is a ground-moving model driven by a `MoveToController`; it turns towards its heading at `turnSpeed`. `ray` provides `makeCameraRay` and `intersectGround` for turning mouse clicks into ground positions.
- `GpuMesh` / `GpuTexture` upload data to the GPU. `GpuMesh` expands every triangle into three unshared vertices and computes face normals when a corner has no normal.
- `Scene` holds a `vector<MeshInstance>` plus `ModelInstance`s added with `Scene::addModel`. A `MeshInstance` has a shared `Mesh`, a `Transform`, a `Material` (colour + optional texture path or embedded image), a `visible` flag, demo animation fields (`initialRotation`, `rotationSpeed`), and an optional link to the `ModelInstance` whose transform it shares.
- Loaders: `obj_loader` (quads/convex polygons, normals, UVs), `gltf_loader` (cgltf; returns a `Model` of `ModelPart`s with world transforms, base colour and base colour texture, external or embedded in `.glb`). glTF texture transforms are not supported.

Software renderer (reference path, used only by tests): `Renderer`, `Display`, `PixelBuffer`, `DepthBuffer`, `rasterizer`, `clipper`, `shading`. It does flat shading only and ignores per-vertex normals and UVs.

## Conventions

- **Math:** `Mat4` is row-major `values[row][column]` and multiplies column vectors (`M * v`). Right-handed coordinates, camera looks down -Z. `Mat4::perspective` produces OpenGL-style clip depth (-w..w). `Application::render` applies a depth correction matrix for the GPU's 0..1 range (this is planned to move into the renderer).
- **GPU uniforms:** matrices are transposed to column-major when pushed to the shader (see `GpuDisplay::drawMesh`).
- **UVs:** bottom-left origin (OBJ convention). `loadImage` flips rows vertically by default to match. glTF uses a top-left origin, so glTF textures need `flipVertically = false`. The texture cache is keyed by (path, embedded image buffer, flip setting).
- **Light:** one hardcoded directional light `(-1, 2, 1)`, duplicated in `renderer.cpp` and `assets/shaders/triangle.frag`.
- **Style:** descriptive names, a block comment above each function explaining why, braces on every branch, RAII for SDL resources, exceptions for errors (constructors clean up with `cleanup()` and rethrow). Match the existing style.
- **Branches:** one branch per feature, with descriptive commit messages.

## Tests

- Plain `assert`-based tests, one `testXxx()` function per file in `tests/`.
- **Register every new test** by adding a forward declaration and a call in `tests/test_main.cpp`. The makefile compiles the file automatically, but it will not run unless registered.
- Tests do not need a GPU or window.
- Do not compile with `-DNDEBUG`, because it turns every assert into a no-op.
- Known issue: the glTF tests use a fixed temp directory and throw if it exists. A failed assert leaves it behind and breaks the next run (delete `game_engine_gltf_*` in the system temp folder).

## Planned work (suggested order)

Done: `Input` snapshot class; `Scene::addModel` and glTF textures; ground raycasts and click-to-move; fixed-timestep simulation with render interpolation.

1. (Done, see above.)
2. Keep the simulation free of SDL and GPU types so it can run headless (for tests and a future dedicated server). `Input` still uses `SDL_Scancode`/`SDL_Event`; replace with an engine `Key` enum.
3. Entity/component layer (roadmap step 10), pulled ahead of physics. Move `initialRotation`/`rotationSpeed` out of `MeshInstance` into a spinner component.
4. Split `GpuDisplay` into platform/window, input, and GPU renderer. Move ground clicks, steering and edge-pan mouse position into `Input`.
5. Asset manager with handles (`MeshHandle`, `TextureHandle`) instead of `shared_ptr<Mesh>` keys and path-string lookups every frame.
6. Colliders (sphere/capsule/AABB), general raycasts against objects, mouse picking, kinematic character controller. Full rigid-body dynamics is lower priority.
7. One canonical mesh/vertex format shared by loaders and GPU upload.
8. Camera split: `Camera` holds view/projection only; the control modes (free-fly, FPS, top-down MOBA) become separate controllers. Mode logic currently lives in `Application` and `CameraController`.
9. Later: `Game` interface (`onInit`/`onUpdate`/`onRender`), networking (authoritative server, snapshots, client prediction), FPS and MOBA demos, UI/text, audio, pathfinding.

## Known small issues and ideas

- Add a `release` make target (`-O2`); move small vec/mat operators into headers so they can inline.
- Swapchain present mode is not set (SDL defaults to vsync), so `targetFPS = 240` is effectively capped by the display. Use `SDL_SetGPUSwapchainParameters` for immediate/mailbox if low latency is wanted.
- The vertex shader computes `transpose(inverse(mat3(model)))` per vertex. Compute the normal matrix once per object on the CPU.
- GPU device is created with debug mode hardcoded on.
- Texture sampler uses nearest filtering and no mipmaps.
