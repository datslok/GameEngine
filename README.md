# GameEngine

A game engine written from scratch in C++20, as a learning project. The long-term goal is an engine that can run both an **FPS** and a **MOBA**: the core stays genre-agnostic, and genre-specific code (camera styles, weapons, abilities, netcode) sits on top.

It started as a software rasterizer drawing pixels on the CPU and now renders on the GPU through SDL3's GPU API (Vulkan backend). See [ROADMAP.md](ROADMAP.md) for what has been built and what comes next, and [docs/ENGINE_GUIDE.md](docs/ENGINE_GUIDE.md) for how it all works.

## Features

- GPU rendering with SDL_GPU (Vulkan) and SPIR-V shaders: depth testing, indexed meshes, textures with mipmaps and trilinear plus anisotropic filtering, gamma-correct (linear-light) colour with sRGB textures and swapchain, dithering against banding
- Lighting: directional, point, spot and ambient lights as components, diffuse plus Blinn-Phong specular highlights, per-material shininess, emissive (glowing) materials, frustum culling (each view, camera or shadow, skips what it cannot see), debug drawing (lines, boxes, spheres and capsules drawn over the scene, with an F4 debug view), shadows from every light (point lights as six cube faces, minus the faces that cannot shadow anything on screen, spotlights and directional lights as one view each, all in one depth atlas with tiles sized by each light's reach, so up to 16 point lights and 8 spotlights cast shadows, soft-edged with PCF, and cached: a tile is only redrawn when its light or something in its view moves), up to 64 point lights and 8 spotlights at once, light priority (with more lights than the shader and shadow atlas hold, the brightest ones where the action is win, skipping lights out of view, with hysteresis so shadows do not flicker between similar lights, and a priority a game can set so a light is never dropped while in view)
- Model loading: OBJ, and glTF/GLB with embedded or external textures; optional smooth normals with a crease angle for models that have none
- Entities and components: generational entity handles, sparse-set component storage, systems as plain functions
- An asset manager that loads each file once and hands out typed handles
- Fixed 120 Hz simulation with interpolated rendering, so movement is smooth at any frame rate
- Event-based input with engine-owned key codes, no SDL types in game code
- Three camera modes: first person, MOBA-style top-down with click-to-move, and a free debug camera
- A software rasterizer kept as a tested reference (lines, triangles, clipping, depth buffer)
- Unit tests that run without a GPU or window

## Requirements

The project is developed on **Windows** with **MSYS2** (UCRT64 environment). You need:

| Software | Used for |
|---|---|
| [MSYS2](https://www.msys2.org/) | Shell, `make` and package manager |
| GCC with C++20 support | Compiler (`g++`) |
| SDL3 and SDL3_image | Window, input, GPU API and image loading |
| shaderc (`glslc`) | Compiling GLSL shaders to SPIR-V |
| A GPU and driver with Vulkan support | Running the game (not needed for the tests) |

[cgltf](https://github.com/jkuhlmann/cgltf) is included in `external/cgltf`, so it needs no install.

## Install

1. Install [MSYS2](https://www.msys2.org/) using its installer (the default location is `C:\msys64`).
2. Open the **MSYS2 UCRT64** shell from the Start menu and install the packages:

   ```sh
   pacman -S --needed make \
       mingw-w64-ucrt-x86_64-gcc \
       mingw-w64-ucrt-x86_64-sdl3 \
       mingw-w64-ucrt-x86_64-sdl3-image \
       mingw-w64-ucrt-x86_64-shaderc
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
| `make` | Builds `build/game.exe` and compiles the shaders to `.spv`. Only changed files are recompiled, in parallel on every core |
| `make run` | Builds, then starts the game |
| `make test` | Builds and runs the tests (`build/tests.exe`) |
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

It builds the game, then creates `build/GameEngineDemo.zip` with the exe, every DLL it needs (found automatically, including the image decoders SDL3_image loads at runtime), the assets and a short README with controls. Before zipping it checks that every DLL resolves inside the package or Windows itself. Use `--no-build` to package the existing build. It needs Python 3 (standard library only) and MSYS2 in `C:\msys64` (or pass `--msys-root`).

## Controls

The game starts in first-person mode. F1-F3 switch modes at any time.

| Input | Action |
|---|---|
| **F1** | First-person mode |
| **F2** | MOBA mode |
| **F3** | Free camera mode |
| **Alt+Enter** | Toggle fullscreen |
| **Escape** | Release the mouse |
| **F** | Toggle the flashlight (held in your right hand; in MOBA mode the duck carries it) |
| **L** | Toggle a field of 42 coloured lamps, each with a pillar beside it: more lamps than can have shadows, to show light priority (the 16 lamps that matter most make their pillars cast shadows) |
| **F4** | Debug view: bounding boxes around every mesh, how many meshes culling skipped and how many point lights are in view, lit and shadowed, and how many shadow tiles were drawn, kept from earlier frames or skipped (top-left), a marker at each light, and the walk target in MOBA mode |

**First person and free camera**

| Input | Action |
|---|---|
| Left-click | Capture the mouse for mouse look |
| Mouse | Look around |
| W A S D | Move |
| Space | Jump (first person only) |
| Space / Ctrl | Move up / down (free camera only) |
| Shift (hold) | Run in first person, fly twice as fast in free camera |

**MOBA**

| Input | Action |
|---|---|
| Left-click | Confine the cursor to the window |
| Right-click (or hold) | Move the character to the cursor |
| Cursor at the screen edge | Pan the camera |
| Space | Lock or unlock the camera on the character |

## Project layout

```
src/            Engine and game source, in layers (lower ones never include higher ones):
  app/          main: picks a game and starts the engine
  games/        Games built on the engine (games/demo is the current demo)
  engine/       The main loop, the Game interface and fixed-tick updates
  gameplay/     Character movement, spinners and camera controllers
  platform/     The window and SDL input translation
  render/gpu/   GPU renderer (SDL_GPU), and the packing of lights and materials for the shaders
  render/software/  Software rasterizer, kept as a tested reference
  scene/        Meshes, models, materials, lights, the camera, render components, interpolation and the frame description
  assets/       The asset manager, and OBJ, glTF and image loaders
  input/        Engine keys and the input snapshot (no SDL)
  ecs/          Entities, component storage and the World
  core/         Fixed timestep, sRGB conversions and small shared types
  math/         Vectors, matrices, transforms, rays
tests/          Unit tests (one test function per file, registered in tests/test_main.cpp)
docs/           ENGINE_GUIDE.md: how the engine works and why, for people learning it
assets/
  models/       OBJ and glTF test models
  shaders/      GLSL shaders (compiled to .spv by make)
  textures/     Images
external/cgltf  Vendored glTF loader
build/          Build output (git-ignored)
```

## Troubleshooting

- **The game cannot find a model, texture or shader:** start it from the repository root, or use `make run`.
- **`SDL3.dll` was not found:** add `C:\msys64\ucrt64\bin` to your `PATH` (see above).
- **The glTF tests fail with a "directory exists" error:** an earlier failed run left its temp folder behind. Delete the `game_engine_gltf_*` folders in your temp directory (`%TEMP%`) and run again.
- **`make: command not found` in PowerShell:** `make` is part of MSYS2, not Windows. Use the UCRT64 shell or set the `PATH` as shown above.

## Credits

- [cgltf](https://github.com/jkuhlmann/cgltf) by Johannes Kuhlmann, for glTF parsing
- [SDL3](https://www.libsdl.org/) and SDL3_image
- The duck model is from the [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets)
