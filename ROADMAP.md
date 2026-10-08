# Roadmap

A from-scratch C++20 engine that can host both an **FPS** and a **MOBA**. The core stays genre-agnostic; genre code (camera styles, weapons, abilities, netcode tuning) lives in modules on top.

Each phase ends with a **milestone**: something you can run and see. A phase is done when its milestone works and has tests where it makes sense.

**Principles**
- The simulation runs headless: no SDL or GPU types inside it, so it can run in tests and on a dedicated server.
- The simulation advances in fixed ticks (120 Hz). Rendering interpolates between ticks.
- One branch per feature, tests alongside the code.
- Build it yourself first to learn how it works; reach for a library when the learning is done (for example Dear ImGui for debug UI).

---

## Phase 0: Foundations (done, Sept 17-19 2026)

The original ten-step plan's steps 1-6, built on the CPU before any GPU code. Branch names are given so each step can be found in `git log`.

- [x] **1. Pixel buffer** (`pixel`, `pixelbuffer`, `image-output`)
  - Store, clear, set and retrieve pixels
  - Write the buffer to an image file to check the output
- [x] **2. Display and game loop**
  - SDL window, texture presentation, event handling
  - Configurable frame rate
- [x] **3. 2D rasterization** (`drawing-primitives`)
  - Lines, triangle outlines, filled triangles
  - Clipping, with tests
- [x] **4. Mathematics foundation** (`math-foundation`)
  - `Vec2`, `Vec3` and `Vec4`; vector arithmetic
  - Dot and cross products
  - Matrices (`Mat4`)
  - Transforms and coordinate spaces
- [x] **5. 3D transformations and camera** (`camera-3d-transformations`, `refactor-camera`)
  - Model, world, view and projection matrices
  - Perspective projection
  - Camera movement
  - Rotating wireframe cube
- [x] **6. 3D rasterization** (`3d-rasterization`, `scene-objects`)
  - Triangle projection
  - Back-face culling
  - Near-plane clipping
  - Barycentric coordinates
  - Depth buffer
  - Scene of multiple objects; flat shading (the software renderer is kept as a tested reference)

**Milestone:** a rotating, depth-tested 3D scene drawn entirely on the CPU. ✔

## Phase 1: GPU rendering and assets (done, Sept 19-27 2026)

Not in the original plan: the engine moved from the software renderer to the GPU here. Also covers most of the original step 7 (shading and lighting).

- [x] **GPU renderer** (`gpu-rendering`)
  - SDL_GPU with the Vulkan backend and SPIR-V shaders compiled by `glslc`
  - Depth texture, per-object uniforms
  - Fullscreen toggle (Alt+Enter)
- [x] **Shading and lighting, part of original step 7**
  - Surface normals, from the file or computed per face
  - Flat shading; smooth shading where the model provides per-vertex normals
  - One directional light with ambient
  - Materials: base colour and optional texture
  - Not done yet (moved to phase 4): point lights, several lights, specular
- [x] **Textures and materials** (`textures-materials`)
  - Texture upload and a texture cache
  - UV conventions: OBJ bottom-left, glTF top-left
- [x] **Model loading** (`gltf-loading`)
  - OBJ loader: polygons, normals, UVs
  - glTF/GLB loader (cgltf): node hierarchy, base colour, external and embedded textures
  - Model bounds and normalization to a target size

**Milestone:** textured glTF models drawn on the GPU. ✔

## Phase 2: Core loop and input (done, Sept 27 - Oct 8 2026)

Covers the input and "separate simulation and rendering" parts of the original step 10, and the fixed timestep from the original step 8.

- [x] **Control modes** (`camera-control-modes`)
  - First person, MOBA and free camera, switched with F1-F3
  - MOBA: confined cursor, edge panning, Space to lock the camera on the player
- [x] **Click-to-move** (`click-to-move`)
  - Ground plane and camera ray picking
  - Right-click (or hold) to move, destination marker, smooth turning
  - Reusable `Character` and `CharacterConfig`
  - `Input` snapshot: held, pressed and released keys; accumulated mouse motion
- [x] **Fixed timestep** (`fixed-timestep`)
  - 120 Hz simulation ticks with an accumulator and a 0.25 s frame time clamp
  - Render interpolation between ticks, with shortest-path angles
  - Input handled once per frame and passed to the simulation as commands
- [x] **Engine `Key` enum** (`engine-key-enum`)
  - `Input` no longer depends on SDL; translation lives in `sdl_input`

**Milestone:** a duck you can steer by clicking, moving smoothly at any frame rate. ✔

### Where the rest of the original plan went

| Original step | Now |
|---|---|
| 7. Shading and lighting: point lights | Phase 4 (rendering upgrades) |
| 8. Particle physics: forces, gravity, integration | Phase 5 (collision and movement) |
| 9. Rigid-body physics: orientation, angular velocity, mass and inertia, collision | Collision in phase 5; full rigid-body dynamics in phase 9 |
| 10. Engine architecture: scene management, entities and components, resource loading | Phase 3 |
| 10. Engine architecture: debug tools | Debug drawing in phase 4; debug UI and profiler in phase 6 |

## Phase 3: Engine architecture ← current

Untangle `Application` and the old `GpuDisplay` before adding more features, so everything after this has a clear place to go.

- [x] Entity/component layer (`entity-component`): entities with generations, sparse-set component storage, `World::each` systems; `Transform`, `PreviousTransform`, `ModelRenderer`, `Spinner` and `CharacterMovement` components replace `Scene`, `MeshInstance` and `ModelInstance`
- [x] Split `GpuDisplay` (`platform-split`) into `Window`, `GpuRenderer` and a richer `Input` (mouse buttons with exact click position and timestamp, cursor, window size); ground clicks, steering and edge panning moved into game code
- [x] `Game` interface (`game-interface`): `onInit`, `onInput`, `onFixedUpdate`, `onUpdate`; `Application` is the engine loop and the demo is `DemoGame`
- [x] Camera split (`camera-split`): `Camera` is a lens (position, orientation, FOV, aspect) with matrices built on request; free-fly, first-person and MOBA are separate concrete controllers, with look angles stored as yaw/pitch
- [x] Asset manager with handles (`asset-manager`): `MeshHandle` and `TextureHandle`, each file loaded once, GPU copies indexed like the handles, no per-frame path lookups
- [ ] One canonical vertex format shared by the loaders and GPU upload; indexed meshes instead of three unshared vertices per triangle

**Milestone:** the current demo runs as a `Game`, and a test runs its simulation for 1000 ticks with no window. âœ” (`tests/demo_game_test.cpp`; the remaining items finish the phase)

## Phase 4: Rendering upgrades

Finishes the old "shading and lighting" step and adds the tools the next phases need to see what they are doing.

- [ ] Light data in a uniform buffer instead of hardcoded in the shader; several directional and point lights
- [ ] Specular highlights (Blinn-Phong)
- [ ] Normal matrix computed once per object on the CPU; move the depth range correction into the renderer
- [ ] Linear filtering and mipmaps
- [ ] Shadow mapping for the main directional light
- [ ] Debug drawing: lines, boxes, spheres and capsules
- [ ] Frustum culling
- [ ] `release` build target (`-O2`)
- [x] Low-latency presentation (`platform-split`): one frame in flight, configurable present mode, mailbox by default

**Milestone:** a lit scene with shadows, and a toggle that draws every collider and bounding box.

## Phase 5: Collision and movement

What both genres actually need from physics: knowing what you hit and moving a character through a level without passing through walls.

- [ ] Shapes: sphere, capsule, axis-aligned box; overlap tests between them
- [ ] Raycasts against shapes and against triangle meshes
- [ ] Mouse picking: click a unit or object, not only the ground
- [ ] Broad phase: uniform grid, so tests only check nearby objects
- [ ] Static level collision against triangle meshes
- [ ] Kinematic character controller: capsule, sliding along walls, stepping up stairs, ground snapping, gravity and jumping
- [ ] Particle physics: forces, gravity, semi-implicit Euler integration, bouncing off level geometry (projectiles, grenades)

**Milestone:** walk around a small level in first person with walls, stairs and jumping, and throw a grenade that bounces.

## Phase 6: Animation, UI and audio

- [ ] Skeletal animation from glTF skins: joints, skinning in the vertex shader, playing and blending clips
- [ ] Text rendering with a bitmap font
- [ ] HUD: crosshair, health bars above units, ability cooldowns
- [ ] Debug UI and in-game stats (frame time, tick time, entity count)
- [ ] Key bindings: games read actions (`MoveForward`, `LockCamera`) instead of keys, and players can rebind them
- [ ] Audio with SDL3: play sounds, volume, simple 3D panning by distance and direction
- [ ] Simple profiler: time each system per frame

**Milestone:** an animated character that walks and idles, with a HUD, sounds and a debug overlay.

## Phase 7: Networking

Builds on the fixed tick and the headless simulation. Commands go up, snapshots come down.

- [ ] Serialization of commands and snapshots into compact byte buffers
- [ ] UDP sockets, connections, and a small reliability layer for messages that must arrive
- [ ] Dedicated server build that runs the simulation headless
- [ ] Authoritative server: clients send commands, the server simulates and sends snapshots
- [ ] Snapshot interpolation for other players' entities
- [ ] Client-side prediction and reconciliation for the local player
- [ ] Lag compensation: the server rewinds hitboxes to what the shooter saw (FPS)
- [ ] Simulated latency and packet loss for testing

**Milestone:** two clients move around the same level through a dedicated server, with smooth movement under 100 ms of simulated latency.

## Phase 8: Genre demos

**FPS**
- [ ] Hitscan and projectile weapons, recoil and spread
- [ ] Health, damage, death and respawn
- [ ] First-person weapon model and camera feel (bob, field of view)

**MOBA**
- [ ] Pathfinding: A* on a grid first, then a navigation mesh
- [ ] Unit selection and attack-move commands
- [ ] Abilities with cooldowns, mana and targeting (point, unit, area)
- [ ] Minion waves with simple AI, towers
- [ ] Fog of war

**Milestone:** a small online deathmatch, and a 1v1 lane with minions and towers.

## Phase 9: Later and optional

- [ ] Parent/child entity hierarchy: attach entities to others (a weapon to a hand, a turret to a vehicle)
- [ ] Full rigid-body dynamics: orientation, angular velocity, mass and inertia, collision response, stacking
- [ ] Physically based materials (glTF metallic-roughness)
- [ ] Particle effects for visuals (sparks, smoke, ability effects)
- [ ] GPU instancing for many identical units
- [ ] Multithreading: a job system for simulation and asset loading
- [ ] Level editor or scene file format
- [ ] Scripting for gameplay code
