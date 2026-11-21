# Engine core

Covers: `src/engine/application`, `src/engine/engine_settings`, `src/engine/state_hash`, `src/core/fixed_timestep`, `src/core/job_system`, `src/core/background_work`, `src/ecs/`, `src/input/`, `src/platform/window`, `src/platform/sdl_input`, `src/core/`, `src/math/`, `src/scene/interpolation`, `assets/shaders/hash.glsl`

The loop, its timing, threads, entities and input. Rules for working here: `src/engine/CLAUDE.md`, `src/ecs/CLAUDE.md`.

## Live path

`main` → game from `app/game_registry` (`--game`, light lab by default) → `Application` drives it through the `Game` hooks, fills `Input` from the `Window`, draws the `World` with `VulkanRenderer`.

## Application

Owns, in declaration order (destroyed in reverse):

| Member | Why there |
|---|---|
| `game` (borrowed) | Declared before the engine by the caller, so it outlives it |
| `settings`, `defaultSettings` | `captureDefaultsAndLoad` applies `settings.cfg` before anything reads a start-up value |
| `jobs` | Borrowed by the renderer and the game |
| `window`, `renderer` | Renderer destroyed first, before the window it draws into; the device outlives every mesh |
| `input`, `world`, `assets` | |
| `debugUi` | Destroyed before the renderer it draws with |

**Start** (each step timed, `core/startup_clock`: one "Start-up" line in the log at the first frame drawn, the renderer's own "Renderer start" line, the first frame's renderer stages):

1. Settings, the window (made hidden, shown with the first loading frame: `Window::reveal`, so the renderer's seconds of start show no black window) and the renderer.
2. `game.onInit` behind the loading screen ("LOADING THE WORLD") on a thread of its own (`workBehindLoadingScreen`, `runWhileWaiting` in `core/background_work`): safe since games are headless, the job system takes work from any thread and nothing else reads the world meanwhile.
3. Every mesh up to the GPU, counted ("PREPARING THE SCENE n / m"), a 33 ms slice of batches between loading frames (`stepsWithinBudget`, a step a batch: their clusters built on the job system, `mesh-clusters.md`), on the main thread, since uploads share the GPU queue with the screen.
4. `waitForPipelines` ("BUILDING SHADERS" until every pipeline is built), then the command server, so `--send` waits for a ready game.
5. The first frame's `describeFrame` and sky bake (`bakeSkyVisibilityAhead`) behind the loading screen on a thread of its own (`describeFirstFrame`), the same work render() would do there.

Closing the window during a stage cannot stop it: drawing stops and the game quits once it ends.

**Each frame:**

1. Commands (`runCommands`), before the frame's time is measured; a paused simulation runs no ticks and draws at alpha 1.
2. Events into `Input`; `updateDebugUi` (F5) before `game.onInput`, so a changed setting applies this frame.
3. `game.onInput` → commands for the simulation.
4. Ticks: `runFixedUpdate` (below).
5. `game.onUpdate` (camera, per-frame motion), then render at `alpha`.

Also each frame: upload what the `AssetManager` gained (games may load at any time); apply `wantsMouseLook()` only when it changes (re-applying would undo Escape); free the mouse while F5 is open.

## Ticks and the budget

- `FixedTimestep` turns frame time into whole ticks (120 Hz) plus `alpha`; a frame longer than `maxFrameSeconds` is clamped.
- `fitsTickBudget`: the first tick always runs; another only if one more average tick fits in `maxTickSecondsPerFrame` (1/60 s). The rest are dropped, not owed, so an over-budget simulation runs in slow motion instead of spiralling.
- GPU cloth ticks cost the CPU nothing but stretch the frame, so with `clothOnGpu` each tick is also charged `gpuClothTickSeconds` (`engine/gpu_cloth`): the last timed frame's `GpuPass::Cloth` over its `RenderStats::clothTicks`, a frame or two late.
- Only how many ticks run depends on the clock, never what a tick does.
- In a tick: `savePreviousTransforms`, then the game's `onFixedUpdate` (the demo: characters, first-person bodies, spinners, fireflies, particles, grenades, in that order).
- Spinners and fireflies pose from total simulation time, so no error builds up.

## Interpolation

- Entities that move continuously have a `PreviousTransform`; `getRenderTransform(world, entity, alpha)` lerps position and scale and slerps rotation (short way).
- Teleporting things have none and draw at `Transform`. Create moving entities with `PreviousTransform` equal to their start.

## Determinism

Same build + same commands → bit-identical state.

- `hashSimulationState` hashes every ticked component field by field (`Fnv1aHash`): `Transform`, `PreviousTransform`, `CharacterController`, `CharacterMovement` (with the move target and yaw), `FirstPersonBody`, `Particle`, `Grenade`.
- `determinism_test`: a scripted demo and light lab run in two worlds, hashes compared every frame; a third world one action short must differ.

## Settings structs

- `EngineSettings` (`engine/engine_settings.h`, owned by `Application`): loop timing, presentation, threads, validation, cloth on GPU, still draws and their cells (`CellSettings`), the command server, and `RenderSettings render`. Each field's meaning and when it is read: the header.
- `RenderSettings` (`render/gpu/render_settings.h`): lights, shadows, bloom, tone mapping, display, global illumination and the rest, with `rayTracing` the master switch; passed to `VulkanRenderer::render` each frame.
- Camera controllers own theirs (`gameplay/camera/`).
- Each has `visit(settings, SettingsVisitor&)` beside it; F5, `settings.cfg` and commands walk that one list (Settings and commands).
- Games add theirs through `Game::visitSettings`; they cannot reach `EngineSettings` (it includes `render/` headers).

## Job system

`JobSystem` (`core/job_system`): worker threads with work stealing, owned by `Application`, borrowed by the renderer and games (`Game::useJobs`).

- **Threads:** `workerThreadCount(settings)` workers (one per core but one; 0 = all on the caller). The calling thread works too.
- **`parallelFor(count, grain, work)`:** halves the range until a piece holds at most `grain` indices; pushes upper halves to the running thread's pile, continues with the lower. No allocation per call (`RangeFunction`, `RangeTask` on the caller's stack). The first exception is rethrown after every piece ends.
- **Stealing:** each thread's `Pile` is a mutex-guarded deque on its own cache lines. Take own newest (back), else another's oldest (front, usually the biggest half). A waiting caller runs anyone's jobs, so nested `parallelFor` finishes. Threads outside the system share pile 0.
- **Sleeping:** workers spin 50 µs, then sleep on a condition variable. Push counts its job before checking for sleepers; a worker counts itself before checking for jobs, so no wake-up is lost.
- **Determinism:** results are thread-independent when each index writes only its own output; sums add fixed chunks in order, then the chunk totals in order.

## Entities and components

- `Entity`: index + generation; handles to destroyed entities go stale even after reuse; `Entity{}` means none.
- `World` holds one sparse-set `ComponentStorage<T>` per type (packed `components`, owning `entities`, `sparse` index).
- `world.each<A, B>(...)` walks `A`'s packed array: list the rarest first.
- **Change logs** (`logChanges<T>(on)`, `takeChanges<T>()`, `isLoggingChanges<T>()`): while on, each entity gaining or losing a `T` (removed or destroyed, as applied) is recorded in order until taken; values changed in place are not. For code that keeps something built between frames (the still draws), so it looks at what changed, never at every component. Off drops the log.
- **During a walk:** `destroy` and `remove<T>` are queued (`PendingChange`) and applied when the outermost walk ends (also on a throw); `create` is immediate; `add<T>` on a walked type throws, `addLater<T>` queues. Tested in `deferred_changes_test`.
- **Components by folder:** `math/` `Transform` (`getMatrix` = T·R·S), `PreviousTransform`; `scene/` `ModelRenderer` (a list of `RenderPart`s: mesh, material, local transform; built by `makeModelRenderer` or `makeMeshRenderer`), lights, `SkyBackground`; `gameplay/` `Spinner`, `Firefly`, `CharacterMovement`, `FirstPersonBody`, `Grenade`; `physics/` `Collider`, `CharacterController`, `Particle`.

## Input and window

- `Input`: a once-a-frame snapshot, no SDL. Keys by engine `Key` (physical US positions): `isKeyHeld`, `wasKeyPressed`, `wasKeyReleased`; mouse buttons alike, each press also a `MouseButtonPress` with exact position and time; `getMouseDelta`, cursor, window size.
- `sdl_input` is the only SDL-to-engine translation (`keyFromScancode`, `mouseButtonFromSdl`, `applySdlEvent`; drops key repeat).
- `Window` (`platform/`) owns SDL video: made hidden and shown by `reveal` once drawn in (not with `--hidden`); events into `Input`, quit, focus loss (releases all), click to capture (mouse look) or confine (MOBA), Escape releases, Alt+Enter fullscreen.
- With F5 open, ImGui sees every event first, and keys and clicks it wants never reach `Input`.
- Mouse motion is forwarded only while captured and dropped when capture or control mode changes.

## Maths and core utilities

Conventions (row-major `Mat4`, column vectors, right-handed, unit `Quat`s): root `CLAUDE.md`. Everything here depends on nothing above `math/`.

| Header | What |
|---|---|
| `math/vec2.h`, `math/vec3.h`, `math/vec4.h`, `math/quat.h` | Defined in the header so they inline (`docs/architecture/build.md`) |
| `math/mat4.h` | `transformPoint`, `transformDirection`, `affineInverse` (model matrices), general `inverse` (projections), `normalMatrix`, `Mat4::perspective` (OpenGL clip depth; the renderer converts) |
| `math/transform.h` | `Transform` (`getMatrix` = T·R·S), `interpolate` (lerp, slerp the short way) |
| `math/aabb.h`, `math/ray.h`, `math/frustum.h`, `math/closest_points.h`, `math/delaunay.h` | Bounds, rays, culling planes, segment and triangle distances, Bowyer-Watson triangulation |
| `math/angles.h`, `math/smooth_step.h` | `fullTurnRadians`, `radiansFromDegrees`, `wrapAngle` (to -pi..pi); `smoothStep`, GLSL's smoothstep, for every C++ mirror |
| `scene/interpolation.h` | `getRenderTransform` (Interpolation above) |
| `core/hash.h`, `hash.glsl`, `hash64.glsl` | `Fnv1aHash` (change checks, the simulation hash), `mixBits` (before using a hash's bits as random numbers; mirrored for shaders with 64-bit whole numbers, numbers in `hash_shared.h`), `pcg3d` (GPU-friendly, mirrored) |
| `core/profiler.h` | `Profiler`, `timeSection`: CPU sections averaged over `defaultProfileWindowSeconds` for F4 |
| `core/half_float.h`, `core/raw_image.h`, `core/pixel.h` | Half floats, CPU images and their PNG packing, `Pixel` |
| `core/build_info.h` | `isDebugBuild` (the makefile defines `ENGINE_DEBUG_BUILD` in debug builds) |
| Others in `core/` | Each with its system's file: commands and settings visitor (`settings-and-commands.md`), dither (`image-pipeline.md`), blue noise (`ambient-occlusion.md`), coverage mips (`materials.md`), height maps (`parallax-and-grounds.md`), image comparison and float tolerance (`testing.md`) |
