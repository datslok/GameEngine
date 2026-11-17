# Source layout

Folders under `src/`, lowest layer first. A folder includes only from folders in lower layers (enforced by the docs test). Includes are written from the `src` root with the folder, never `../`: `#include "math/vec3.h"`, `#include "cgltf/cgltf.h"` (include roots `-Isrc -Iexternal`).

| Layer | Folder | Holds | Includes |
|---|---|---|---|
| 0 | `math/` | Vectors, `Mat4`, `Quat`, `Transform`, `Ray`, `Frustum`, `Aabb`, angles, Delaunay | — |
| 0 | `ecs/` | `Entity`, `ComponentStorage`, `World` | — |
| 1 | `core/` | Timestep, profiler, hashing, images, settings visitor, commands, job system | `math` |
| 1 | `input/` | `Input`, keys, mouse (no SDL) | `math` |
| 2 | `physics/` | Shapes, overlaps, raycasts, static level, character controller, particles, signed distance fields | `math`, `ecs`, `core` |
| 3 | `scene/` | Meshes, materials, lights, sky, camera, culling, LOD, terrain, wind, frame description | below, `physics` |
| 4 | `assets/` | `AssetManager`, loaders (glTF, OBJ, images, stars) | below, `scene` |
| 5 | `cloth/` | Cloth solvers, collision, patterns, seams, tethers | below, not `assets` |
| 5 | `gameplay/` | Character movement, camera controllers, control modes, flashlight | below |
| 6 | `render/` | `gpu/` (API-free, mirrored maths), `vulkan/` (the renderer), `software/` (test reference) | below but `assets`, `input`, `gameplay` |
| 6 | `platform/` | Window, SDL input, command socket | `input` |
| 7 | `engine/` | `Application`, `Game` interface, settings file, debug UI, commands, golden images | below |
| 8 | `games/` | `demo`, `lightlab`, `clothlab`, `forest`: one folder a game | below but `render`, `platform` (games run headless) |
| 9 | `app/` | `main`, game registry, launch options | everything |

**Known exception:** `scene/impostor.h` includes `assets/image_loader.h` for `ImageData` (to be fixed: move `ImageData` below `scene`).

Each system's design: `docs/ARCHITECTURE.md`.
