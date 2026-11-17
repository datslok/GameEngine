# Source layout

Folders under `src/`, lowest layer first. A folder includes only from folders in lower layers, and never from those it lists under "Never includes". Includes are written from the `src` root with the folder, never `../`: `#include "math/vec3.h"`, `#include "cgltf/cgltf.h"` (include roots `-Isrc -Iexternal`).

`tests/docs_test.cpp` reads the table and the exceptions below and checks every include in `src/` against them: keep their format.

| Layer | Folder | Holds | Never includes |
|---|---|---|---|
| 0 | `math/` | Vectors, `Mat4`, `Quat`, `Transform`, `Ray`, `Frustum`, `Aabb`, angles, Delaunay | |
| 0 | `ecs/` | `Entity`, `ComponentStorage`, `World` | |
| 1 | `core/` | Timestep, profiler, hashing, images, settings visitor, commands, job system | |
| 1 | `input/` | `Input`, keys, mouse (no SDL) | |
| 2 | `physics/` | Shapes, overlaps, raycasts, static level, character controller, particles, signed distance fields | |
| 3 | `scene/` | Meshes, materials, lights, sky, camera, culling, LOD, terrain, wind, frame description | |
| 4 | `assets/` | `AssetManager`, loaders (glTF, OBJ, images, stars) | |
| 5 | `cloth/` | Cloth solvers, collision, patterns, seams, tethers | `assets/` |
| 5 | `gameplay/` | Character movement, camera controllers, control modes, flashlight | |
| 6 | `render/` | `render/gpu/` (API-free, mirrored maths), `render/vulkan/` (the renderer), `render/software/` (test reference) | `assets/`, `input/`, `gameplay/` |
| 6 | `platform/` | Window, SDL input, command socket | |
| 7 | `engine/` | `Application`, `Game` interface, settings file, debug UI, commands, golden images | |
| 8 | `games/` | `demo`, `lightlab`, `clothlab`, `forest`: one folder a game | `render/`, `platform/` |
| 9 | `app/` | `main`, game registry, launch options | |

Folders not in the table (`cgltf/`, `imgui/`, `volk/` and the rest) are libraries in `external/`.

## Known exceptions

- `scene/impostor.h` includes `assets/image_loader.h`: `ImageData` is to move below `scene/`.

Each system's design: `docs/ARCHITECTURE.md`.
