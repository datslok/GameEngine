# GameEngine

A from-scratch C++20 game engine. Long-term goal: support both **FPS** and **MOBA** style games by keeping the core genre-agnostic and putting genre-specific code (camera controllers, weapons, abilities, netcode style) in separate modules on top.

The roadmap is in `ROADMAP.md`. Phases 0-2 (foundations, GPU rendering and assets, core loop and input) are done; phase 3 (engine architecture) is in progress: the entity/component layer, the platform split, the `Game` interface and the camera split are done.

## Build and run

- `make` builds `build/game.exe` and compiles shaders (`glslc` -> `.spv`).
- `make run` builds and runs the game. Run it from the repo root: asset and shader paths are relative (`assets/...`).
- `make test` builds and runs `build/tests.exe`.
- The makefile finds `src/**/*.cpp` recursively (`rwildcard`) and `tests/*.cpp`, so new files are picked up automatically. Headers and the makefile are tracked as dependencies. Include roots are `-Isrc -Iexternal`.
- Flags are `-std=c++20 -Wall -Wextra -g` with no optimisation (-O0). A release target with `-O2` is a planned improvement.
- Dependencies: SDL3, SDL3_image, `glslc` (MSYS2 `shaderc` package here; the Vulkan SDK's also works), and cgltf (vendored in `external/cgltf`).
- `build/`, `*.exe` and `*.spv` are git-ignored.
- Source files use CRLF line endings. Preserve them when editing.

## Source layout

Code lives in layered folders under `src/`. Dependencies point downward only: a folder may include from folders below it, never above.

- `ecs/` (`Entity`, `ComponentStorage`, `World`), `core/` (`fixed_timestep`, `pixel`) and `math/` (vectors, `Mat4`, `Transform`, `Ray`): depend on nothing else.
- `input/` (`Key`, `Input`): SDL-free, depends on `math/`.
- `scene/` (meshes, models, materials, `ModelRenderer`, `interpolation`, `Camera`, `camera_ray`) and `assets/` (loaders, cgltf).
- `gameplay/` (`CharacterMovement`, `Spinner`, `MoveToController`, `edge_pan`) and `gameplay/camera/` (`look_controls`, `FreeFlyCameraController`, `FirstPersonCameraController`, `MobaCameraController`).
- `platform/` (`Window`, `sdl_input`), `render/gpu/` (`GpuRenderer`, `GpuMesh`, `GpuTexture`), `render/software/` (reference rasterizer). `render/` takes a raw `SDL_Window*` and never includes `input/` or `platform/`.
- `engine/` (`Application`, the `Game` interface, `runFixedUpdate`): the loop and everything shared by all games. Never includes `games/`.
- `games/` (`demo/DemoGame`, `demo/ControlMode`): one folder per game, built on the `Game` interface. Games never include `platform/`, `render/` or SDL, so they can run headless.
- `app/` (`main`): picks a game and starts the engine; may include everything.

Write includes from the `src` root with the folder: `#include "math/vec3.h"`, `#include "cgltf/cgltf.h"`. Never use relative `../` includes.

## Architecture (current)

Live path: `main` creates a `DemoGame` and an `Application` (the engine), which drives the game through the `Game` hooks, fills `Input` from the `Window`, and draws the `World` with the `GpuRenderer`.

- `Application` (`engine/`) runs the main loop (fixed timestep, frame cap) for a borrowed `Game` (declare the game first so it outlives the engine). It owns the `Window`, `GpuRenderer`, `Input`, the `World`, and a map from shared `Mesh` to uploaded `GpuMesh`; new meshes and textures are uploaded before the frame they first appear in, so games can spawn at any time. It applies `game.wantsMouseLook()` to the window only when it changes (re-applying would undo Escape).
- `Game` (`engine/game.h`) hooks, called each frame in this order: `onInput(world, input)` once, `onFixedUpdate(world, tickSeconds, simulationSeconds)` per tick (no `Input` parameter on purpose), `onUpdate(world, input, frameSeconds, alpha)` once; plus `onInit(world)`, `getCamera()` (the engine sets its aspect ratio) and `wantsMouseLook()`. Always run ticks through `runFixedUpdate`, which saves previous transforms first; tests use it too. `DemoGame` (`games/demo/`) holds the camera, control modes, player and marker entities, and all demo input rules. `window` is declared first and `renderer` second: members are destroyed in reverse order, so the renderer goes before the window it draws into, and the GPU device outlives every uploaded mesh.
- `Window` (`platform/`) owns SDL video and the `SDL_Window`: event polling into `Input`, quit, focus loss (releases everything), click to capture (mouse look) or confine (MOBA cursor), Escape to release, Alt+Enter fullscreen. After the events it records the cursor position, window size, keyboard focus, mouse capture and cursor confinement into `Input`, so games read those facts from `Input` instead of the window. It reports raw facts only; game rules live in game code.
- `GpuRenderer` (`render/gpu/`) uses SDL3's GPU API (SDL_GPU) with the Vulkan backend and SPIR-V shaders, drawing into a window it does not own. It owns the device, pipeline, depth texture and texture cache (`prepareMaterial`, `beginFrame`, `drawMesh`, `endFrame`).
- `Input` is a once-per-frame snapshot with no SDL dependency: `isKeyHeld`, `wasKeyPressed`, `wasKeyReleased` take an engine `Key` (`key.h`, physical US-layout positions), plus accumulated `getMouseDelta`. Mouse buttons (`MouseButton`) work like keys, and each press is also recorded as a `MouseButtonPress` with the exact event position (window pixels) and timestamp (ns, `SDL_GetTicksNS` clock): use `getLastMouseButtonPress` for clicks, `getCursorPosition`/`isCursorInWindow`/`getWindowSize` for continuous things. It is filled through `pressKey`/`releaseKey`/`pressMouseButton`/`releaseMouseButton`/`addMouseMotion`/`setCursor`/`setWindowSize`. `sdl_input` is the only SDL-to-engine translation (`keyFromScancode`, `mouseButtonFromSdl`, `applySdlEvent`, which also drops key repeat). `Window::processEvents(input)` resets and fills it. Mouse motion is only forwarded while the mouse is captured, and is discarded when capture or control mode changes. Game code reads keys from `Input` as `Key` values, never SDL scancodes or `SDL_GetKeyboardState`.
- Entities and components (`ecs/`): an `Entity` is an index plus a generation (handles to destroyed entities go stale even after their slot is reused; `Entity{}` is never alive and means "none"). `World` creates/destroys entities and stores one `ComponentStorage<T>` (sparse set: packed `components`, owning `entities`, and a `sparse` index) per component type. Components are plain structs; systems are free functions run with `world.each<A, B>([](Entity, A&, B&) { ... })`, which walks the first type's packed array, so list the rarest component first. Inside `each`, do not add/remove components of the listed types or destroy entities.
- Components today: `Transform` (`math/`), `PreviousTransform` and `ModelRenderer` (`scene/`), `Spinner` and `CharacterMovement` (`gameplay/`). `ModelRenderer` is a list of `RenderPart`s (mesh, material, `localTransform` with the model's normalization folded in) plus `visible`; a multi-part model is one entity. Build with `makeModelRenderer(model, normalization)` or `makeMeshRenderer(mesh, material)`.
- Main loop: `FixedTimestep` (120 Hz, frame time clamped to 0.25 s) turns real frame time into a whole number of ticks plus an interpolation `alpha`. Each frame: `game.onInput` turns input into commands, `runFixedUpdate` runs zero or more ticks, `game.onUpdate` moves the camera, then `render(alpha)`. Never read `Input` inside a tick; key/click edges are handled once per frame and reach the simulation as commands. Mouse look and camera movement are per frame, not per tick.
- Each tick: `savePreviousTransforms` (engine), then `DemoGame::onFixedUpdate` runs `updateCharacters` and `updateSpinners` in that order. Interpolation: only entities that move continuously get a `PreviousTransform`; `getRenderTransform(world, entity, alpha)` blends it with `Transform` (`interpolate` takes the shortest way around for angles), and entities without one are drawn at their `Transform`. Things that teleport (the destination marker) therefore have no `PreviousTransform`. Create moving entities with `PreviousTransform` equal to their starting `Transform`.
- Cameras: `Camera` (`scene/`) is a plain lens: position, forward, up, vertical FOV, aspect, near/far; view and projection matrices are built on request, so `setVerticalFov` works (aim-down-sights zoom). It has no movement logic. Controllers in `gameplay/camera/` move it, each a concrete class with no shared interface (a real game uses one): `FreeFlyCameraController` (look + fly, Space/Ctrl up/down), `FirstPersonCameraController` (look + walk on the ground plane), `MobaCameraController` (fixed-angle offset; follows an optional target, otherwise edge-pans while the cursor is confined). Look controllers store yaw/pitch as `ViewAngles` (spherical coordinates, yaw 0 = -Z, positive yaw turns right, pitch clamped to 89 degrees) and pause while the mouse is not captured; `takeOver(camera)` continues from the camera's current direction.
- Control modes (`ControlMode`, demo only): F1 first-person, F2 MOBA, F3 free camera (debug switching, the `DemoGame` constructor's `debugModeSwitching`). MOBA mode uses a confined visible cursor, edge panning (`gameplay/edge_pan`, from the cursor in `Input`), Space to lock the camera on the player, and right-click (or hold) to move the player (`DemoGame::updatePlayerCommands`: a click uses its exact event position; holding steers towards the live cursor via `groundSteeringActive`).
- Characters: `spawnCharacter(world, model, config, position)` creates an entity with `Transform`, `PreviousTransform`, `ModelRenderer` and `CharacterMovement` (a `MoveToController` plus turning state; `moveTo`/`stop`/`isMoving`). `updateCharacters` moves them and turns them towards their heading at `turnSpeed`; `getCharacterVisualCentre(world, entity, alpha)` gives the interpolated camera target. `math/ray` provides `Ray` and `intersectGround`; `scene/camera_ray` provides `makeCameraRay`. Together they turn mouse clicks into ground positions.
- `GpuMesh` / `GpuTexture` upload data to the GPU. `GpuMesh` expands every triangle into three unshared vertices and computes face normals when a corner has no normal.
- Loaders: `obj_loader` (quads/convex polygons, normals, UVs), `gltf_loader` (cgltf; returns a `Model` of `ModelPart`s with world transforms, base colour and base colour texture, external or embedded in `.glb`). glTF texture transforms are not supported.

Software renderer (reference path, used only by tests): `Renderer`, `Display`, `PixelBuffer`, `DepthBuffer`, `rasterizer`, `clipper`, `shading`. It does flat shading only and ignores per-vertex normals and UVs.

## Conventions

- **Math:** `Mat4` is row-major `values[row][column]` and multiplies column vectors (`M * v`). Right-handed coordinates, camera looks down -Z. `Mat4::perspective` produces OpenGL-style clip depth (-w..w). `Application::render` applies a depth correction matrix for the GPU's 0..1 range (this is planned to move into the renderer).
- **GPU uniforms:** matrices are transposed to column-major when pushed to the shader (see `GpuRenderer::drawMesh`).
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

## Planned work

`ROADMAP.md` is the single source of truth for planned work, grouped into phases with milestones. Tick items off there (`- [x]`) as part of the feature branch that completes them, and keep this file's architecture section in sync.

Notes for upcoming work:

- Never read `Input` inside `simulate`; key and click edges are handled once per frame and reach the simulation as commands.
- Not built yet: parent/child entity hierarchies (attach a weapon to a hand). Listed in ROADMAP.md phase 9.

## Known small issues and ideas

- Add a `release` make target (`-O2`); move small vec/mat operators into headers so they can inline.
- Latency: `GpuRenderer` allows one frame in flight, and `Application` requests `PresentMode::Mailbox` (falls back to vsync if unsupported), so `targetFPS = 240` is the real frame cap. `PresentMode::Immediate` is lower latency still but tears.
- The vertex shader computes `transpose(inverse(mat3(model)))` per vertex. Compute the normal matrix once per object on the CPU.
- GPU device is created with debug mode hardcoded on.
- Texture sampler uses nearest filtering and no mipmaps.
