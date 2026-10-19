# GameEngine

A game engine written from scratch in C++20, as a learning project. The long-term goal is an engine that can run both an **FPS** and a **MOBA**, and eventually a dressmaking game with real cloth physics: the core stays genre-agnostic, and genre-specific code (camera styles, weapons, abilities, netcode) sits on top.

It began life as a software rasterizer drawing pixels on the CPU and now renders on the GPU with its own Vulkan 1.3 renderer. See [ROADMAP.md](ROADMAP.md) for what has been built and what comes next, [docs/BUILDLOG.md](docs/BUILDLOG.md) for how each step went, and [docs/ENGINE_GUIDE.md](docs/ENGINE_GUIDE.md) for how it all works.

## Features

- GPU rendering with SPIR-V shaders on the engine's own Vulkan 1.3 renderer (volk, vk-bootstrap and VMA; dynamic rendering, synchronization2 and hand-written barriers; two frames in flight by default, so the CPU records the next frame while the GPU draws this one, or one for the least latency, switchable while running), which replaced SDL_GPU drawing every golden image bit for bit the same; a GPU scene (every object, material and light in storage buffers written once per frame, every texture in one bindless array, and one instanced draw per mesh in each view, so the lamp field's 161 visible objects take 10 draw calls; shadow casters are first culled against each light's range sphere); a depth prepass (the scene's depth drawn first, so each pixel is shaded once, for the nearest surface only, whatever order objects are drawn in; switchable in the debug UI) and lights behind a surface skipped before their shadow lookups; clustered shading (a compute pass splits the view into a grid of clusters and lists, as one bit per light, the lights that can reach each, so a pixel loops only over its own cluster's lights and up to 1024 point lights and 256 spotlights cost only where they shine; the picture is identical to the last bit with it on and off, and a heatmap debug view shows how many lights each cluster lists); hardware ray tracing on GPUs that have it (ray queries: any shader asks what a ray hits first, against an acceleration structure per mesh, compacted, and one rebuilt from every object each frame; a debug view traces one ray per pixel and shows normals, albedo or front and back faces; the foundation for global illumination, optional, so GPUs without it simply leave it off); global illumination (DDGI: light that bounces): a grid of light probes placed by each game, each tracing 64 rays a frame, 16 frames taking turns through one long spiral so they cover every direction evenly between them (as calm as three times the rays), and blending what they bring back (sky, and surfaces lit by the sun and the brightest nearby lamps, found by shadow rays, plus the light the probes held last frame, so each frame adds a bounce) into small octahedral images of the light arriving from every direction and the distance to the nearest surface, smoothed over time by time constants in seconds so they behave the same at any frame rate, with an optional ray budget (probe updates per second) above which each frame traces a share of the rays, so the probe work stops growing with the frame rate; light from fast-moving lights (a flashlight, headlights) is kept in a second set of images with a memory of a twentieth of a second, the probes near such a light tracing 256 rays, so a swung flashlight's bounce follows it instead of trailing behind for seconds; every pixel blends its 8 nearest probes, weighted by nearness and by whether each probe can see it at all (a statistical test on its stored distances), so sunlight through a window lights the whole room, warmly where it bounces off the floor, without leaking through walls into the next; with a debug view of a ball at each probe showing the light it holds; reversed-Z depth (near at 1, far at 0, so depth precision is nearly even out to the far plane) and camera-relative rendering (positions measured from the camera before they become floats in matrices, so distant scenes do not jitter), indexed meshes, textures with mipmaps and trilinear plus anisotropic filtering, gamma-correct (linear-light) colour with sRGB textures and swapchain, HDR rendering (the scene is lit in 16-bit floats, then tone mapped with a choice of curves, AgX, ACES, Reinhard or plain clamping, and an exposure, so bright lights roll off smoothly instead of clipping to white), bloom (a few percent of all light scattered into a soft halo, as in a camera lens: the scene halved down a chain of levels with Jimenez's 13-tap filter and grown back with tent filters, energy-conserving, with a firefly filter), HDR output on HDR monitors (scRGB, with paper white and peak brightness settings in nits, falling back to SDR when Windows HDR is off), dithering against banding
- Lighting: directional, point, spot and ambient lights as components, physically based materials (glTF metallic-roughness: GGX microfacet highlights with Smith shadowing and Schlick Fresnel, Lambert diffuse, and energy compensation for light that bounces between microfacets, so nothing is lost or created at any roughness), emissive (glowing) materials, frustum culling (each view, camera or shadow, skips what it cannot see), debug drawing (lines, boxes, spheres and capsules drawn over the scene, with an F4 debug view), shadows from every light (point lights as six cube faces, minus the faces that cannot shadow anything on screen, spotlights as one view each, the sun as four cascades (box views from sharp near the camera to coarse out to 200 m, or only as far as the farthest surface on screen, measured on the GPU each frame, so in a room every texel is 7 to 17 times finer; sized so turning never makes them shimmer, blended at their seams), all in one depth atlas with tiles sized by each light's reach, so up to 64 point lights and 8 spotlights cast shadows, with soft edges that widen with distance from the blocker like real ones (PCSS, sized by each light's glowing size, and for the sun by its half-degree disc), and cached: a tile is only redrawn when its light or something in its view moves), up to 1024 point lights and 256 spotlights at once, light priority (with more lights than the shader and shadow atlas hold, the brightest ones where the action is win, skipping lights out of view, with hysteresis so shadows do not flicker between similar lights, and a priority a game can set so a light is never dropped while in view)
- Model loading: OBJ, and glTF/GLB with embedded or external textures, metallic and roughness (factors and texture), normal maps (with the file's tangents, or tangents generated with MikkTSpace as baking tools do, so the bumps look as their authors made them, mirrored pictures included) and emissive colour (with KHR_materials_emissive_strength); optional smooth normals with a crease angle for models that have none
- Entities and components: generational entity handles, sparse-set component storage, systems as plain functions that may destroy entities and remove components mid-walk (queued until the walk ends)
- An asset manager that loads each file once and hands out typed handles
- Fixed 120 Hz simulation with interpolated rendering, so movement is smooth at any frame rate (orientations are quaternions, blended along the shortest turn); deterministic (the same commands give a bit-identical result, checked by a test that plays the demo twice), ready for replays and networking
- Event-based input with engine-owned key codes, no SDL types in game code
- Three camera modes: first person (a body that walks, jumps, crouches and sprints through the level with Overwatch-like movement), MOBA-style top-down with click-to-move, and a free debug camera, shared by every game that wants them
- Several games on one engine, picked at start-up (`--game`): the demo (a night scene with lamps, floodlights, a gallery of normal-mapped models and an obstacle course) and the light lab (a sunlit test ground 800 m across: towers at doubling distances, a thin fence running to the horizon, a forest and a two-room building lit through one window, under a sun placed by real astronomy, from latitude, day of the year and time of day, that can run through a day, with its glowing disc in the sky at its real size and as bright as the light it gives)
- A software rasterizer kept as a tested reference (lines, triangles, clipping, depth buffer)
- Collision shapes: spheres, capsules and axis-aligned boxes with exact overlap tests between every pair, built on closest-point queries between points, segments and boxes; raycasts against the shapes and against triangle meshes, with hit distance, point and surface normal; `Collider` components (a shape plus optional exact triangles) and mouse picking that finds the nearest collider under the cursor in two stages, shapes first and triangles to confirm; a uniform grid broad phase so queries only test colliders near them; static level collision: the level's triangles in their own grid, built once, with raycasts and capsule contacts (how deep, and which way out); a kinematic character controller tuned to feel like Overwatch: walls slide, steps and ramps are climbed, the ground is followed down, gravity has air drag up to a terminal speed, and jumps and air steering use Overwatch's measured numbers; sphere sweeps (the first touch of a moving ball, so nothing fast can pass through a wall) and particles: balls that fly on exact parabolas, bounce with restitution and Coulomb friction, slide to a stop and rest without jitter (the demo throws them as grenades)
- Settings in one place: every tunable value (simulation rate, frame cap, light and shadow tuning, bloom, tone mapping, HDR output, camera speeds and mouse sensitivity, grenade physics) lives in a settings struct owned by its system and is described once by a `visit()` next to it (label, limits, unit), which drives everything else: a debug UI (F5, Dear ImGui: a slider, checkbox or list for every setting, quality presets Low, Medium, High and Ultra (the default: the sun's cascades at 4096), Save and Reset buttons, drawn over the image in its own layer so it looks right in HDR too), a settings file (`settings.cfg`, only the values changed from the defaults, loaded at start-up, forgiving of hand edits) and tests that every default lies within its limits; numbers the shaders share with C++ are written once
- Profiling: frame, work, tick, update, scene-building and render times, and what each demo system costs per tick, as the average and worst call over each half second, plus the GPU's own time for each render pass (the ray tracing scene's build, the light probes, cluster light lists, shadows, depth prepass, main, the ray tracing debug view, the depth measurement for the sun's cascades, bloom, tone mapping, debug lines, debug UI) measured with GPU timestamps, in the F4 view; render passes and images carry names, so RenderDoc and Nsight show the frame as named passes
- Unit tests that run without a GPU or window, and golden images: fixed views of each game rendered once as references (the scene image in half floats and the final image, SDR and HDR) that later captures must match pixel for pixel, with a difference heatmap when they do not
- A GPU test tier: the shaders' maths lives in libraries of pure functions that the render passes and small compute shaders share, and `make gputest` runs those functions on the GPU over thousands of fixed inputs and compares them with their tested C++ mirrors, within tolerances sized for how GPUs round (and, for sharp highlights, for how sensitive the maths itself is), reporting how much of each tolerance the worst value used; then the golden images

## Requirements

The project is developed on **Windows** with **MSYS2** (UCRT64 environment). You need:

| Software | Used for |
|---|---|
| [MSYS2](https://www.msys2.org/) | Shell, `make` and package manager |
| GCC with C++20 support | Compiler (`g++`) |
| SDL3 and SDL3_image | Window, input, GPU API and image loading |
| shaderc (`glslc`) | Compiling GLSL shaders to SPIR-V |
| A GPU and driver with Vulkan support | Running the game (not needed for the tests) |

[cgltf](https://github.com/jkuhlmann/cgltf) is included in `external/cgltf` and MikkTSpace in `external/mikktspace`, so they need no install.

## Install

1. Install [MSYS2](https://www.msys2.org/) using its installer (the default location is `C:\msys64`).
2. Open the **MSYS2 UCRT64** shell from the Start menu, update MSYS2 (run this twice: the first run updates the core and closes the shell, so reopen it), and install the packages:

   ```sh
   pacman -Syu --noconfirm
   pacman -S --needed make \
       mingw-w64-ucrt-x86_64-gcc \
       mingw-w64-ucrt-x86_64-sdl3 \
       mingw-w64-ucrt-x86_64-sdl3-image \
       mingw-w64-ucrt-x86_64-shaderc \
       mingw-w64-ucrt-x86_64-vulkan-headers \
       mingw-w64-ucrt-x86_64-vulkan-validation-layers
   ```

3. Clone the repository:

   ```sh
   git clone https://github.com/datslok/GameEngine.git
   cd GameEngine
   ```

`glslc` from the Vulkan SDK also works if it is on your `PATH`.

## Build and run

Run these from the **repository root** in the UCRT64 shell. The game loads assets and shaders by relative paths (`assets/...`), so it must be started from there.

| Command | What it does |
|---|---|
| `make` | Builds `build/game.exe` and compiles the shaders to `.spv`. Only changed files are recompiled, in parallel on every core. This is the unoptimised debug build, best for a debugger |
| `make run` | Builds, then starts the game (the light lab; `make run GAME=demo` starts the demo, and `GAME=` works for `make run-release`, `make golden` and `make golden-save` too) |
| `make test` | Builds and runs the tests (`build/tests.exe`) |
| `make release` | Builds the optimised release (`-O2`) into `build/release/`. Physics runs about 7 times faster than in the debug build; use it to play and to measure |
| `make run-release` | Builds the release, then starts it |
| `make test-release` | Builds and runs the tests optimised, which can catch bugs the debug build hides |
| `make golden` | Renders every game's golden views and compares them with the references in `tests/golden/<GPU>/<game>/` (needs a GPU; captures and difference heatmaps go to `build/golden/<GPU>/<game>/`) |
| `make gputest` | Runs every shader library's functions in compute shaders on the GPU and compares them with their C++ mirrors (`build/gputests.exe`), then the golden images (needs a GPU; `make gputest-release` for the optimised build) |
| `make golden-save` | Renders the golden views and saves them as the new references (only on purpose: after a change that is meant to alter the picture) |
| `make clean` | Removes the compiled objects, executables and shaders |

New `.cpp` files in `src/` and `tests/` are picked up automatically.

### Building from PowerShell or another terminal

Outside the UCRT64 shell, put the MSYS2 tools first on your `PATH` for that session:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:PATH
make run
```

The game also needs `C:\msys64\ucrt64\bin` on the `PATH` when you start it directly, because that is where `SDL3.dll` and the other runtime libraries live.

## Sharing the demo

To send the demo to someone without MSYS2, run:

```sh
python tools/package_demo.py
```

It builds the game, then creates `build/GameEngineDemo.zip` with the exe, `Demo.bat` (starts the exe with `--game demo`; the exe alone starts the light lab), every DLL it needs (found automatically, including the image decoders SDL3_image loads at runtime), the assets and a short README with controls (copied from the Controls section below, so it never falls behind). Before zipping it checks that every DLL resolves inside the package or Windows itself. Use `--no-build` to package the existing build. To share it anonymously, add `--anonymous`: it makes `build/Demo.zip` with a neutral name, a README with only short control names (nothing that matches this repo's text), every zip entry dated 1 January 1980 and the exe's build time zeroed. It needs a real Python 3 (standard library only; `winget install Python.Python.3.13`; the `python` in `WindowsApps` is only the Microsoft Store placeholder) and MSYS2 in `C:\msys64` (or pass `--msys-root`).

## Controls

The game starts in first-person mode. F1-F3 switch modes at any time.

The light lab (the default game, `--game lightlab`; the exe alone in the zip) has the same controls for moving and looking (F1-F5) and the flashlight (F, off at the start, held in your right hand in every mode: shine it into the dark back room to watch its bounce light follow it: a fast light, its bounce kept apart with a short memory); the lamps (L) and grenades (G) are the demo's only. Its sun is under Light lab, Sun in the settings window (F5): latitude, day of the year, solar time, hours per second to run the clock, and the sun disc's size (1 is the real half degree). Light lab, Probes, Dense probes switches the light probes from 1 m apart round the building to 20 cm apart inside it, a test of finer bounce light (about 20 times the probe work).

| Input | Action |
|---|---|
| **F1** | First-person mode |
| **F2** | MOBA mode |
| **F3** | Free camera mode |
| **Alt+Enter** | Toggle fullscreen |
| **Escape** | Release the mouse |
| **F** | Toggle the flashlight (held in your right hand; in MOBA mode the duck carries it) |
| **L** | Toggle a field of 100 coloured lamps (more than the 64 point light shadows, so light priority has to choose which lamps cast them) that fly round their pillars like fireflies, so the pillars' shadows sweep round: a stress test for light priority and shadows (every shadow tile is redrawn each frame while its light moves) |
| **K** | Toggle the four floodlights over the obstacle course (on at the start): shadow-casting spotlights on poles whose crossing beams fill in each other's shadows, like stadium lights |
| **F4** | Debug view: timings in pale green (frame, work without the frame cap's sleep, one tick, the camera update, building the scene and rendering, which includes waiting for the GPU, as average/worst milliseconds; then the GPU's time for the whole frame and for each pass, averaged; then each simulation system's average cost per tick), bounding boxes around every mesh, collider wireframes in orange, how many colliders the hover ray tested out of all of them (the broad phase at work), the level triangle under the cursor with its normal and how many level triangles that ray tested, the duck's ground normal, slope and movement substeps, how many meshes culling skipped and how many draw calls each view took (CALLS), how many point lights are in view, lit and shadowed, and how many shadow tiles were drawn, kept from earlier frames or skipped, with how far the sun's cascades reach (SUN TO), and the video memory in use against the driver's budget with the engine's own share (VRAM, top-left), a marker at each light, and the walk target in MOBA mode |
| **F5** | Settings window (debug UI): a slider, checkbox or list for every engine and demo setting, grouped by system (light priority, shadows, bloom, tone mapping, HDR output, frame cap, cameras, grenades), a quality preset (Low, Medium, High, Ultra), Save (writes the settings you changed to `settings.cfg`, loaded at every start) and Reset to defaults. Greyed-out settings are read only at start-up. The mouse is freed while it is open, and clicks on the window do not reach the game |

**First person and free camera**

| Input | Action |
|---|---|
| Left-click | Capture the mouse for mouse look |
| Mouse | Look around |
| W A S D | Move: in first person you walk through the level at Overwatch's speeds (5.5 m/s, 90% backwards), climbing steps and ramps and stopped by walls |
| Space | Jump (first person; a fresh press each time) |
| G | Throw a grenade (first person): it flies along your view, bounces off floors, walls and steps, slides to a stop and is gone after 5 seconds |
| Ctrl (hold) | Crouch in first person (3 m/s, stays crouched under low ceilings); move down in free camera |
| Space / Ctrl | Move up / down (free camera only) |
| Shift (hold) | Sprint in first person (1.5 times), fly twice as fast in free camera |
| F1 from free camera | Drop into first person where the camera is, and fall to whatever is below |

**MOBA**

| Input | Action |
|---|---|
| Mouse over an object | Outline it faintly (hover) |
| Left-click | Confine the cursor to the window; once confined, select the object under the cursor (outlined in cyan; clicking bare ground deselects). Objects are picked by their exact triangles, so a click through the gap in the teapot's handle misses it |
| Right-click (or hold) | Move the character to the cursor: it walks over the level, up steps and ramps (no path finding yet, so walls stop it) |
| Cursor at the screen edge | Pan the camera |
| Space | Lock or unlock the camera on the character |

## Project layout

```
src/            Engine and game source, in layers (lower ones never include higher ones):
  app/          main and the game registry: picks a game by name and starts the engine
  games/        Games built on the engine: games/demo (the demo) and games/lightlab (the sunlit test ground)
  engine/       The main loop, the Game interface, fixed-tick updates, the debug UI and the settings file
  gameplay/     Character movement, spinners, fireflies, grenades, camera controllers and the player controls games share (F1-F3)
  platform/     The window and SDL input translation
  render/gpu/   The renderer's parts that do not call Vulkan: physically based shading and normal mapping (and their tested CPU mirrors), shadow atlas and caching, the sun's cascades and their fit to the screen's depths, soft shadows, light priority, debug lines, the overlay the debug UI is drawn into, quality presets, the GPU scene: the instance, material and light records the shaders read, and draws batched by mesh, the depth tests of the depth prepass and main pass, clustered shading's grid, light tests and heatmap colours, ray tracing's instances, masks and debug view camera, and the light probes' maths (octahedral images, ray directions, blending, the atlas layout, and sampling them at surfaces: the cell, the visibility and back-face weights, the lights kept at rays' hits)
  render/vulkan/ The engine's own Vulkan 1.3 renderer: device (with or without a window), swapchain, barriers, graphics and compute pipelines, per-frame uniforms and descriptors, the bindless texture array and the scene's buffers and layout, ray tracing's acceleration structures, and every pass (the light probes' rays (with their bounce and lamps) and update and the clusters' light lists, compute passes; shadows, depth prepass, main, the probes' debug balls, the ray tracing debug view, the depth measurement, bloom, tone mapping, debug lines, the debug UI)
  render/software/  Software rasterizer, kept as a tested reference
  scene/        Meshes (with tangents for normal maps), models, materials, lights, light probe volumes, the sun's position in the sky, the camera, render components, interpolation and the frame description
  assets/       The asset manager, and OBJ, glTF and image loaders
  input/        Engine keys and the input snapshot (no SDL)
  ecs/          Entities, component storage and the World
  core/         Fixed timestep, profiler, sRGB conversions, the settings visitor, float tolerances and small shared types
  physics/      Collision shapes, overlap tests, raycasts, colliders, picking, the broad phase grid, static level collision, sphere sweeps, particles and the character controller
  math/         Vectors, matrices, quaternions, transforms, rays, boxes, closest points
tests/          Unit tests (one test function per file, registered in tests/test_main.cpp)
  golden/       Golden image references, one folder per GPU and in it one per game (see tests/golden/README.md)
  gpu/          The GPU test tier (make gputest): the runner, one test per shader library, and their compute shaders in gpu/shaders
docs/           ENGINE_GUIDE.md: how the engine works and why, for people learning it
                ARCHITECTURE.md: the detailed reference of every system (names, files, numbers)
                BUILDLOG.md: the history of finished roadmap items (when, why, measurements, dead ends)
                ILLUMINATION.md: research notes for global illumination (DDGI papers, formulas, numbers)
                RAGDOLLPLAN.md: plan and research notes for active ragdolls (roadmap phase 8)
tools/          package_demo.py (the shareable zip), make_glb_demo.py
assets/
  models/       OBJ and glTF test models (the normal map gallery: Khronos's CompareNormal, NormalTangentTest and NormalTangentMirrorTest)
  shaders/      GLSL shaders (compiled to .spv by make) and the shader libraries they include (.glsl: BRDF, normal mapping, lighting, soft shadows, cascades, depth range, ray tracing, tone mapping, bloom, dither, overlay, clusters)
  textures/     Images
external/cgltf  Vendored glTF loader
external/imgui  Vendored Dear ImGui (the debug UI), with its SDL3 and Vulkan backends
external/mikktspace  Vendored MikkTSpace (tangents for normal maps, C)
external/volk   Vendored volk (loads Vulkan's functions from the driver, C)
external/vk-bootstrap  Vendored vk-bootstrap (Vulkan instance, GPU and device set-up)
external/vma    Vendored Vulkan Memory Allocator
build/          Build output (git-ignored)
```

## Troubleshooting

- **The game cannot find a model, texture or shader:** start it from the repository root, or use `make run`.
- **`SDL3.dll` was not found:** add `C:\msys64\ucrt64\bin` to your `PATH` (see above).
- **The glTF tests fail with a "directory exists" error:** an earlier failed run left its temp folder behind. Delete the `game_engine_gltf_*` folders in your temp directory (`%TEMP%`) and run again.
- **`make: command not found` in PowerShell:** `make` is part of MSYS2, not Windows. Use the UCRT64 shell or set the `PATH` as shown above.

## Credits

- [cgltf](https://github.com/jkuhlmann/cgltf) by Johannes Kuhlmann, for glTF parsing
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut, for the debug UI
- [MikkTSpace](https://github.com/mmikk/MikkTSpace) by Morten S. Mikkelsen, for tangents (zlib licence)
- [SDL3](https://www.libsdl.org/) and SDL3_image
- The duck model and the normal map gallery are from the [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets): CompareNormal by Eric Chadwick and DGG (CC0), NormalTangentTest and NormalTangentMirrorTest by Ed Mackey, © 2018 Analytical Graphics, Inc. ([CC BY 4.0](https://creativecommons.org/licenses/by/4.0/))
