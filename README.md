# Mewton

Mewton, a game engine written from scratch in C++23, as a learning project: its own Vulkan 1.3 renderer, physics, cloth and tools, on a genre-agnostic core meant to host an **FPS** and a **MOBA**, and later a dressmaking game with real cloth, a real-scale solar system and a flying superhero.

- [ROADMAP.md](ROADMAP.md): what is built and what comes next
- [docs/ENGINE_GUIDE.md](docs/ENGINE_GUIDE.md): how it works and why, for people learning it
- [docs/INDEX.md](docs/INDEX.md): every doc, including how each system works (`docs/architecture/`)

## Features

**Renderer** ([renderer](docs/architecture/renderer.md))

- Its own Vulkan 1.3 renderer (volk, vk-bootstrap, VMA, dynamic rendering, hand-written barriers); two frames in flight; pipelines compiled on every core behind a loading screen.
- A GPU scene: objects, materials and lights in storage buffers, every texture bindless, one instanced draw per mesh per view; still objects' draws kept between frames.
- Depth prepass, clustered shading (1024 point and 256 spot lights, each pixel walking only its cluster's), reversed-Z, camera-relative positions.
- Level of detail by screen error, impostors for far trees, card thinning and dithered cross-fades instead of popping, CDLOD terrain reaching tens of kilometres.
- BC7-compressed textures, a quarter of the video memory, kept in `build/texture_cache` (the first start encodes them, later starts read them).

**Lighting and shadows** ([lights](docs/architecture/lights.md), [sun shadows](docs/architecture/sun-shadows.md))

- Physically based materials (glTF metallic-roughness, energy-conserving GGX), clear coat, light through leaves, parallax occlusion mapping, blended and triplanar-projected grounds, cut-outs, glass with refraction, frosted glass and coloured shadows.
- Shadows from every light in one cached atlas, soft by distance to the blocker (PCSS); the sun in four cascades fitted to the screen's depths, a far map to 5 km, contact shadows.
- Light priority and hysteresis when lights outnumber the slots.

**Light that bounces** ([ambient occlusion](docs/architecture/ambient-occlusion.md), [sky visibility](docs/architecture/sky-visibility.md), [reflections](docs/architecture/reflections.md))

- Without ray tracing: screen-space ambient occlusion and bounce light (a GTAO visibility bitmask), a sky visibility grid baked on the CPU so rooms go dark, screen-space reflections, a reflection probe at the camera, planar mirrors drawn for real, mirrored lamps as image lights.
- With ray tracing (off by default, one switch): DDGI light probes in cascades round the camera (on hold), ray-traced reflections, light reflected by mirrors (manifold next event estimation), tinted glass shadows.

**Sky, air and image** ([sky](docs/architecture/sky.md), [fog](docs/architecture/fog.md), [image](docs/architecture/image-pipeline.md))

- A physical sky and atmosphere (Hillaire 2020), the sun, moon and 9,096 real stars placed by astronomy, volumetric fog in layers (haze, mist pooled under a canopy) lit by every light.
- TAA with upscaling, FSR 3.1, DLSS (its Quality mode by default); eye adaptation, bloom, AgX, ACES or Reinhard tone mapping, HDR output; depth of field, motion blur and a camera finish (off by default).

**Simulation** ([engine core](docs/architecture/engine-core.md), [physics](docs/architecture/physics.md), [cloth](docs/architecture/cloth.md))

- Entities and components, a deterministic 120 Hz simulation with interpolated drawing, a work-stealing job system.
- Collision shapes, raycasts, a static level, sphere sweeps, particles, and a character controller tuned to Overwatch's movement; first-person, MOBA and free cameras.
- Cloth (parked): XPBD and vertex block descent with coarse levels, tethers, sewing patterns into garments, self-collision, all on the GPU too; distance fields for bodies of any shape.
- Wind: a force field of springs, trees, plants and grass swaying by kind, hanging things swinging.

**Tools** ([settings and commands](docs/architecture/settings-and-commands.md), [testing](docs/architecture/testing.md))

- Every setting described once: a debug UI (F5), `settings.cfg`, quality presets, a typical GPU setup, and commands to a running game from the command line.
- F4 profiling with CPU stages and GPU timestamps per pass; debug drawing.
- Unit tests without a GPU, golden images, and a GPU tier checking every shader library against its C++ mirror.

**Games** ([games](docs/architecture/games.md)), picked with `--game`:

- `lightlab` (default): a sunlit test ground with a sun clock, a street of lamps, a two-room building, mirrors, glass and mist.
- `demo`: a night scene with flying lamps, floodlights, glTF test models and an obstacle course.
- `clothlab`: cloth stations, a mannequin and a sewn dress.
- `forest`: a jungle valley kilometres across, with grown trees, palms, lianas, floating islands and brutalist megatowers lit at night (windows, floodlights, flashing red aircraft-warning lights) (being built).

## Requirements

Windows with **MSYS2** (UCRT64):

| Software | Used for |
|---|---|
| [MSYS2](https://www.msys2.org/) | Shell, `make`, packages |
| GCC with C++23 | Compiler |
| SDL3, SDL3_image | Window, input, images |
| shaderc (`glslc`) | GLSL to SPIR-V |
| Vulkan headers and validation layers | The renderer, and checking it in debug builds |
| A Vulkan GPU | Running the game (not the tests) |

Everything else is in `external/`. Only DLSS's DLLs need fetching.

## Install

1. Install [MSYS2](https://www.msys2.org/) (default `C:\msys64`).
2. In the **MSYS2 UCRT64** shell, update twice (the first run closes the shell) and install:

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

3. `git clone https://github.com/datslok/GameEngine.git` and `cd GameEngine` (the repository keeps its first name).
4. Optional, needing a real Python:
   - DLSS on RTX GPUs: `python tools/fetch_streamline.py` (about 276 MB).
   - The forest's scanned assets: `python tools/fetch_forest_assets.py` (45 CC0 assets, about 270 MB; without them its ground is plain colours).

## Build and run

From the **repository root** (assets load by relative paths):

| Command | What it does |
|---|---|
| `make` / `make run` | Debug build (`build/game.exe`) / build and run |
| `make release` / `make run-release` | Optimised build (`build/release/`, about 7 times faster physics) / build and run |
| `make test-release` | The tests, optimised; `make test` for debug |
| `make golden`, `make gputest` | Golden images and GPU tests (need a GPU) |
| `make yardstick-quick` | The cloth solver's measurements |
| `make clean` | Remove builds and shaders |

`GAME=<name>` picks the game for `make run`, `make run-release` and `make golden`. Only changed files rebuild, on every core. More: [build](docs/architecture/build.md).

From PowerShell, put MSYS2 first on the `PATH` (the game needs its DLLs there too):

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:PATH
make run
```

### Controlling a running game

A running game takes commands on 127.0.0.1:47613 (your computer only):

```sh
build/game.exe --send help                                  # every command
build/game.exe --send set engine.render.bloom.strength 0.1  # live, by its settings.cfg key
build/game.exe --send camera 0 2 10 0 1 0                   # stand at 0 2 10, look at 0 1 0
build/game.exe --send screenshot build/shots/a.png
```

Also `get`, `list`, `reset`, `preset`, `typical-gpu`, `save-settings`, `pause`, `resume`, `step`, `debug-view`, `cull-compare`, `placement-compare`, `stats`, `wait`, `quit`. At start: `--set key=value`, `--run script.txt`, `--port`, `--hidden`, `--typical-gpu`. Details: [settings and commands](docs/architecture/settings-and-commands.md).

## Sharing the demo

`python tools/package_demo.py` builds `build/MewtonDemo.zip` for a PC without MSYS2: the exe, a launcher per game, every DLL it needs (checked), the assets and a README with the controls below. `--anonymous` makes a neutral `build/Demo.zip`; `--no-build` packages the existing build.

## Controls

The game starts in first person; F1-F3 switch modes. Every game shares the camera controls; the lamps and grenades are the demo's.

| Input | Action |
|---|---|
| **F1** | First-person mode |
| **F2** | MOBA mode |
| **F3** | Free camera mode |
| **Alt+Enter** | Toggle fullscreen |
| **Escape** | Release the mouse |
| **F** | Toggle the flashlight in the demo, the light lab and the forest (off at the start; in MOBA mode the duck carries it) |
| **L** | Toggle the light lab's back-room ceiling lamp, or the demo's field of 100 coloured lamps |
| **K** | Toggle the light lab's street lights, or the demo's four floodlights (on at the start) |
| **Left mouse (held)** | Drag the cloth lab's sheet where the crosshair points, within 10 m |
| **F4** | Debug view: timings, GPU passes, light counts, video memory; bounding boxes and colliders with "Debug bounding boxes" on (F5) |
| **F5** | Settings window: every setting, quality presets, the typical GPU setup, Save and Reset |

**First person and free camera**

| Input | Action |
|---|---|
| Left-click | Capture the mouse for mouse look |
| Mouse | Look around |
| W A S D | Move (first person walks at Overwatch's speeds, climbing steps and ramps) |
| Space | Jump (first person) |
| G | Throw a grenade (first person) |
| Ctrl (hold) | Crouch in first person; move down in free camera |
| Space / Ctrl | Move up / down (free camera) |
| Shift (hold) | Sprint in first person; fly twice as fast in free camera |
| F1 from free camera | Drop into first person where the camera is |

**MOBA**

| Input | Action |
|---|---|
| Mouse over an object | Outline it faintly |
| Left-click | Confine the cursor; then select the object under it |
| Right-click (or hold) | Walk the character to the cursor |
| Cursor at the screen edge | Pan the camera |
| Space | Lock or unlock the camera on the character |

## Project layout

- `src/`: the engine and games in layers ([docs/LAYOUT.md](docs/LAYOUT.md)).
- `tests/`: unit tests, `tests/gpu/` the GPU tier, `tests/golden/` the reference images.
- `assets/`: shaders, models, textures, the star catalogue.
- `docs/`: the guide, the build log, research notes and `docs/architecture/` ([docs/INDEX.md](docs/INDEX.md)).
- `tools/`: packaging, fetching assets, the visible test runner.
- `external/`: vendored libraries.

## Troubleshooting

- **A model, texture or shader is not found:** start from the repository root, or use `make run`.
- **`SDL3.dll` not found:** put `C:\msys64\ucrt64\bin` on the `PATH`.
- **The glTF tests fail with "directory exists":** delete the `game_engine_gltf_*` folders in `%TEMP%`.
- **`make: command not found` in PowerShell:** use the UCRT64 shell, or set the `PATH` as above.

## Credits

- [cgltf](https://github.com/jkuhlmann/cgltf) by Johannes Kuhlmann, for glTF parsing
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut, for the debug UI
- [MikkTSpace](https://github.com/mmikk/MikkTSpace) by Morten S. Mikkelsen, for tangents (zlib licence)
- [meshoptimizer](https://github.com/zeux/meshoptimizer) by Arseny Kapoulkine, for simplifying meshes (MIT licence)
- [volk](https://github.com/zeux/volk), [vk-bootstrap](https://github.com/charles-lunarg/vk-bootstrap) and the [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator), for the renderer's Vulkan setup
- [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) v1.1.4, for FSR 3.1 upscaling (MIT licence)
- **NVIDIA DLSS**, through [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) v2.14.16 (Streamline MIT; DLSS under the NVIDIA RTX SDK licence). NVIDIA, the NVIDIA logo and DLSS are trademarks of NVIDIA Corporation.
- The Yale Bright Star Catalogue, 5th revised edition (Hoffleit and Warren 1991, from the [CDS](https://cdsarc.cds.unistra.fr/viz-bin/cat/V/50)), for the stars (`assets/stars/`)
- The forest's scanned plants, rocks and textures: CC0 from [Poly Haven](https://polyhaven.com/) and [ambientCG](https://ambientcg.com/) (`tools/fetch_forest_assets.py`)
- [SDL3](https://www.libsdl.org/) and SDL3_image
- The duck model and the gallery are from the [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets): WaterBottle by Microsoft (CC0), CompareNormal by Eric Chadwick and DGG (CC0), NormalTangentTest, NormalTangentMirrorTest and AlphaBlendModeTest by Ed Mackey, © 2018 Analytical Graphics, Inc. ([CC BY 4.0](https://creativecommons.org/licenses/by/4.0/))
