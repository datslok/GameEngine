# Build log: Phases 0-5

## Phase 0: Foundations (Sept 17-19 2026)

The original ten-step plan's steps 1-6, built on the CPU before any GPU code:

- pixel buffer (`pixel`, `pixelbuffer`, `image-output`)
- display and game loop
- 2D rasterization with clipping (`drawing-primitives`)
- the maths foundation (`math-foundation`)
- 3D transformations and camera (`camera-3d-transformations`, `refactor-camera`)
- 3D rasterization (`3d-rasterization`, `scene-objects`)

The software renderer was kept as a tested reference after the move to the GPU.

## Phase 1: GPU rendering and assets (Sept 19-27 2026)

Not in the original plan: the engine moved from the software renderer to the GPU here (`gpu-rendering`: SDL_GPU with its Vulkan backend, SPIR-V compiled by `glslc`).

It also covered most of the original step 7 (shading and lighting): normals, flat and smooth shading, one directional light with ambient, base colour and textures. Point lights, several lights and specular highlights moved to phase 4.

Then:

- `textures-materials`: the texture cache, OBJ's bottom-left and glTF's top-left UV conventions.
- `gltf-loading`: OBJ and glTF/GLB through cgltf, model bounds and normalisation to a target size.

## Phase 2: Core loop and input (Sept 27 - Oct 8 2026)

The input and "separate simulation and rendering" parts of the original step 10, and the fixed timestep from the original step 8:

- `camera-control-modes` (F1-F3)
- `click-to-move`
- `fixed-timestep` (120 Hz, 0.25 s clamp, interpolation, input as commands)
- `engine-key-enum` (`Input` free of SDL)

Where the rest of the original ten-step plan went:

| Original step | Now |
|---|---|
| 7. Shading and lighting: point lights | Phase 4 (rendering upgrades) |
| 8. Particle physics: forces, gravity, integration | Phase 5 (collision and movement) |
| 9. Rigid-body physics: orientation, angular velocity, mass and inertia, collision | Collision in phase 5; rigid bodies and joints in phase 8 (ragdolls); stacking and box contacts in phase 14 |
| 10. Engine architecture: scene management, entities and components, resource loading | Phase 3 |
| 10. Engine architecture: debug tools | Debug drawing in phase 4; profiler in phase 5; debug UI in phase 6a |

## Phase 3: Engine architecture (Oct 8 2026)

Done to untangle `Application` and the old `GpuDisplay` before adding more features, so everything after had a clear place to go.

- `entity-component`: `Transform`, `PreviousTransform`, `ModelRenderer`, `Spinner` and `CharacterMovement` components replaced the old `Scene`, `MeshInstance` and `ModelInstance`.
- `platform-split`: `GpuDisplay` became `Window`, `GpuRenderer` and a richer `Input`.
  - Ground clicks, steering and edge panning moved out of the platform layer into game code.
  - The same branch set up low-latency presentation: one frame in flight, a configurable present mode, mailbox by default (two frames in flight became the default in phase 6b).
- `game-interface`.
- `camera-split`: camera as a lens, controllers separate, look angles as yaw and pitch.
- `asset-manager`: handles, each file loaded once, no per-frame path lookups.
- `indexed-meshes`: `Mesh` stays the import format; the render format merges identical corners.
- Milestone check: `tests/demo_game_test.cpp` runs the demo's simulation for 1000 ticks with no window.

## Phase 4: Rendering upgrades (Oct 8-9 2026)

Finished the old "shading and lighting" step and added the tools later phases needed to see what they were doing.

- `lights-uniform-buffer`, `specular-highlights` (Blinn-Phong, replaced by physically based shading in phase 6).
- `spotlights`: up to 4 at first. The demo became a night scene with an F-toggled flashlight, at the camera or carried by the duck in MOBA mode.
- `moonlight`: emissive materials.
  - The demo got a faint moonlight and a glowing moon sphere you can fly to (`fly-to-moon`).
  - The MOBA marker glows instead of using a temporary unlit flag.
- `smooth-normals`: opt-in per load, because only the game knows whether a shape is meant to be faceted.
- `cpu-normal-matrix`: the normal matrix once per object on the CPU, and the depth range correction moved into the renderer.
- `texture-filtering`: trilinear plus 16x anisotropic, mips generated on the GPU at upload.
- `srgb-colour`: textures had been created as `UNORM`, so lighting, blending and mip averaging treated sRGB values as linear light and came out slightly too dark.
  - sRGB textures and an sRGB swapchain fixed it.
  - The whole scene got brighter, so material and light colours were retuned.
- `frame-description`: the engine hands the renderer the whole frame instead of calling `drawMesh` per object. Shadows needed it (the scene is drawn twice), so did frustum culling, and it is the basis for ray tracing later.
- `shadow-mapping`: one depth atlas.
  - Point lights get six cube-face tiles, spotlights one, directional lights one texel-snapped box around the camera; normal offset plus slope bias and 3x3 PCF.
  - The demo's moon was a shadow-casting point light inside the moon sphere for a while, to try point shadows, then went back to a directional light (`moon-directional`), whose shadows are sharper.
  - Hardware slope-scaled bias was dropped later: it detached shadows from pillars seen edge-on by small tiles.
- Tile sizes by reach (`light-priority`): 256, 512 or 1024 pixels for about 0.04 units per texel, the least important shrinking first when the atlas is full, packed as a quadtree, with room for 16 shadowed point lights and 8 spotlights.
- `debug-drawing`: demo F4 view of bounding boxes, light markers and the walk target; colliders joined it in phase 5. `debug-stats` and `debug-line-renderer` followed.
- `dithering`: slow dark fades, such as the flashlight's pool at night, showed rings in 8 bits.
- `frustum-culling`: each shadow tile culls with its own light view, never the camera's, so off-screen things still cast shadows into view.
- `light-priority`: seats in the shader and shadows had gone to the first lights in component storage, which shuffles when components are removed.
  - Ranking is by brightness at a focus point, `intensity / (d² + r²)` without the range window so far lights do not all tie at zero.
  - Last frame's choices count 1.25 times (hysteresis) so similar lights do not swap every frame.
  - Seats were raised to 64 point and 8 spotlights.
  - The demo got a field of 42 lamps with pillars (later 100, `firefly-lamps`, Oct 11).
- The light and shadow follow-ups were done in a fixed order, each tested in the game before the next started:
  1. `light-manual-priority`: a `priority` on lights, so a game can mark a boss's aura or a story lamp that must never be dropped in view. Only the ranking function changed.
  2. `light-fading`: handovers fade over 0.25 s instead of popping.
     - It came before caching because it changes how slots are handed over.
     - Lights just switched on or newly in view appear at full strength, since nothing showed them before.
  3. `shadow-face-culling`: a cube face whose view misses the camera's view gets no tile. In the lamp field it mostly skipped the face pointing back at the camera, about 16 of 96 faces, freeing atlas room and draw time for caching.
  4. `shadow-caching`: tiles redrawn only when something in them changes. In the lamp field, standing still, about 49 of 82 tiles were cached; the rest saw the spinning objects, the flashlight or the moon box.
     - `more-point-shadows`: the shadow cap went from 16 to 64 point lights, as many as the seats, since the atlas still fit everything at the smallest tile size.
       - `LightLimits` lets tests cap shadows below the seats to keep shadow-only handovers covered.
       - The shader started skipping lights beyond their range, and their 9 shadow lookups.
     - `sharper-spot-shadows` (Oct 18): spotlight beams land on the ground at a slant well short of their reach, which stretched each texel, and the floodlights' step and pole shadows were jagged.
       - Spotlights may now grow to 2048-pixel tiles and aim for 0.02 units per texel.
       - The floodlights got 2048 and the flashlight 512 (it had 256). They are few and usually still, so their cached tiles cost nothing.
  5. `soft-shadows` (PCSS): a new `emitterRadius` replaced `sourceRadius` for shadows, because `sourceRadius` had been tuned as a falloff softener far bigger than any real bulb (4.5 on the demo flashlight).
     - The first values, 0.1 and 0.05, looked too soft and became 0.05 for point lights and 0.025 for spotlights.
     - Directional lights kept the 3x3 filter then, because a real sun's penumbra was smaller than a texel of the 30-unit box (soft sun shadows came with the cascades in phase 6c).
- `faster-builds`: a one-line change took about 76 s (27 s for the game, 49 s for the tests), because everything compiled in one `g++` call.
  - The fix: objects per file, GCC's `-MMD -MP` header dependencies, parallel builds with output kept in order, and engine objects shared by the game and the tests.
  - That brought it to about 2 s, with a clean build of everything about 10 s.

## Phase 5: Collision and movement (Oct 9-18 2026)

What both genres need from physics: knowing what you hit, and moving a character through a level without passing through walls.

- `settings-structs`: done before physics added dozens of new constants.
  - Merged on the way: the first-person and free cameras' shared look-and-move steps (`turnCamera`, `moveCamera`) and angle wrapping (`math/angles`).
  - Numbers the shaders share with C++ moved into macro-only headers both include.
- `collision-shapes`: `Aabb` is the old `ModelBounds`, moved to `math/`.
- `raycasts`: every triangle was tested at first; the grids came after.
- `mouse-picking`: two-stage picking, shapes first and triangles to confirm, because games click simple shapes rather than meshes.
- `broad-phase`: the `CollisionScene` is rebuilt from every `Collider` rather than updated, on purpose.
  - Incremental updates would need change tracking the ECS does not have, and could leave ghost entries.
  - Tested against brute force on random scenes.
- `static-level`: built as pieces are added and never rebuilt.
  - The demo showed rays slipping through the seam between two triangles, so the triangle test allows a tiny margin (kept as a bit-exact regression test).
  - The demo got its obstacle course, whose collision mesh comes from the same parts as what is drawn, and hover picking where walls hide what is behind them.
- `character-controller`, `first-person-body`: tuned to feel like Overwatch from community-measured numbers. The tuning is approved: ask before changing it.
  - The numbers: 5.5 m/s walking, 90% backwards, 3 m/s crouched, a 5.72 m/s jump, 17.5 m/s² gravity with quadratic drag to a 30 m/s terminal speed, slope-consistent speed and speed-keeping air steering from the 2018 movement update.
  - Ground and air acceleration are guesses tuned by feel.
  - Settling onto the ground drops in 0.05 steps, because dropping the whole way at once let a kerb's edge win the push-out and shove the capsule sideways.
  - The body replaced the camera-only walking and the placeholder jump (`fps-jump`).
- `floodlights`: four shadow-casting spotlights over the obstacle course.
- `particle-physics`:
  - Flight uses the average of the start and end velocities, exact for constant gravity, so throws follow the true parabola.
  - Collisions are swept, so nothing passes through a wall at any speed.
  - Particles colliding with each other were left out: piles need phase 14's iterative contact solver.
  - The shared part for cloth and orbits is the per-particle maths. Cloth keeps its own array store (phase 7), and the functions become templates over the vector type when phase 13 adds `DVec3`.
- `profiler` (moved here from the UI phase): physics was the first work whose cost grows with the scene, so the release build and every later performance decision should rest on measurements.
  - It counts per call, since ticks do not fall one per frame, and shows each half-second window's average and worst call, so spikes are not averaged away.
  - RENDER included waiting for the GPU then: SDL_GPU could not time the GPU itself.
- `release-build` (moved here from the optional list). Per tick, 40 characters walking a stepped level and 200 bouncing balls:

  | Build | 40 characters | 200 balls | Against debug |
  |---|---|---|---|
  | debug | 0.52 ms | 0.54 ms | |
  | `-O2` alone | 0.105 ms | 0.090 ms | 5-6 times faster |
  | with the vector operators inlined | 0.075 ms | 0.067 ms | 7-8 times faster than debug |
  | link-time optimisation | 0.073 ms | 0.061 ms | |

  - Link-time optimisation was left out: a one-file rebuild took 20 s instead of 3 s.
  - The `-O2` build also showed a false array-bounds warning inside `std::sort` in `closestPointsSegmentBox`, now an insertion sort of its at most 8 cuts.
- `collision-guide`: the "Collision and movement" chapter of `docs/ENGINE_GUIDE.md`.
