# Build log

The history of the finished roadmap items: when each was built, why, what was measured at the time, and what was tried and dropped. It is kept apart from the plan so `ROADMAP.md` stays a short list of what is done and what comes next.

- `ROADMAP.md`: the plan; finished items there are one line each.
- `docs/ARCHITECTURE.md`: how the code works today (names, files, numbers). Facts about the current code belong there, not here.
- This file: decisions, reasons, measurements and dead ends, in the order they happened, grouped by roadmap phase. Numbers are as measured then, so later work may have changed them. Branch names are given so each step can be found in `git log` (`git log --merges` lists them with dates).

When a roadmap item is ticked off, cut it to one line there and add its story here, in the same branch.

---

## Phase 0: Foundations (Sept 17-19 2026)

The original ten-step plan's steps 1-6, built on the CPU before any GPU code: pixel buffer (`pixel`, `pixelbuffer`, `image-output`), display and game loop, 2D rasterization with clipping (`drawing-primitives`), the maths foundation (`math-foundation`), 3D transformations and camera (`camera-3d-transformations`, `refactor-camera`), and 3D rasterization (`3d-rasterization`, `scene-objects`). The software renderer was kept as a tested reference after the move to the GPU.

## Phase 1: GPU rendering and assets (Sept 19-27 2026)

Not in the original plan: the engine moved from the software renderer to the GPU here (`gpu-rendering`: SDL_GPU with its Vulkan backend, SPIR-V compiled by `glslc`). It also covered most of the original step 7 (shading and lighting): normals, flat and smooth shading, one directional light with ambient, base colour and textures. Point lights, several lights and specular highlights moved to phase 4. Then `textures-materials` (the texture cache, OBJ's bottom-left and glTF's top-left UV conventions) and `gltf-loading` (OBJ and glTF/GLB through cgltf, model bounds and normalisation to a target size).

## Phase 2: Core loop and input (Sept 27 - Oct 8 2026)

The input and "separate simulation and rendering" parts of the original step 10, and the fixed timestep from the original step 8: `camera-control-modes` (F1-F3), `click-to-move`, `fixed-timestep` (120 Hz, 0.25 s clamp, interpolation, input as commands), `engine-key-enum` (`Input` free of SDL).

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
- `platform-split`: `GpuDisplay` became `Window`, `GpuRenderer` and a richer `Input`; ground clicks, steering and edge panning moved out of the platform layer into game code. The same branch set up low-latency presentation: one frame in flight, a configurable present mode, mailbox by default (two frames in flight became the default in phase 6b).
- `game-interface`, `camera-split` (camera as a lens, controllers separate, look angles as yaw and pitch), `asset-manager` (handles, each file loaded once, no per-frame path lookups), `indexed-meshes` (`Mesh` stays the import format; the render format merges identical corners).
- Milestone check: `tests/demo_game_test.cpp` runs the demo's simulation for 1000 ticks with no window.

## Phase 4: Rendering upgrades (Oct 8-9 2026)

Finished the old "shading and lighting" step and added the tools later phases needed to see what they were doing.

- `lights-uniform-buffer`, `specular-highlights` (Blinn-Phong, replaced by physically based shading in phase 6).
- `spotlights`: up to 4 at first. The demo became a night scene with an F-toggled flashlight, at the camera or carried by the duck in MOBA mode.
- `moonlight`: emissive materials. The demo got a faint moonlight and a glowing moon sphere you can fly to (`fly-to-moon`); the MOBA marker glows instead of using a temporary unlit flag.
- `smooth-normals`: opt-in per load, because only the game knows whether a shape is meant to be faceted.
- `cpu-normal-matrix`: the normal matrix once per object on the CPU, and the depth range correction moved into the renderer.
- `texture-filtering`: trilinear plus 16x anisotropic, mips generated on the GPU at upload.
- `srgb-colour`: textures had been created as `UNORM`, so lighting, blending and mip averaging treated sRGB values as linear light and came out slightly too dark. sRGB textures and an sRGB swapchain fixed it. The whole scene got brighter, so material and light colours were retuned.
- `frame-description`: the engine hands the renderer the whole frame instead of calling `drawMesh` per object. Shadows needed it (the scene is drawn twice), so did frustum culling, and it is the basis for ray tracing later.
- `shadow-mapping`: one depth atlas; point lights six cube-face tiles, spotlights one, directional lights one texel-snapped box around the camera; normal offset plus slope bias and 3x3 PCF. The demo's moon was a shadow-casting point light inside the moon sphere for a while, to try point shadows, then went back to a directional light (`moon-directional`), whose shadows are sharper. Hardware slope-scaled bias was dropped later: it detached shadows from pillars seen edge-on by small tiles.
- Tile sizes by reach (`light-priority`): 256, 512 or 1024 pixels for about 0.04 units per texel, the least important shrinking first when the atlas is full, packed as a quadtree, with room for 16 shadowed point lights and 8 spotlights.
- `debug-drawing`: demo F4 view of bounding boxes, light markers and the walk target; colliders joined it in phase 5. `debug-stats` and `debug-line-renderer` followed.
- `dithering`: slow dark fades, such as the flashlight's pool at night, showed rings in 8 bits.
- `frustum-culling`: each shadow tile culls with its own light view, never the camera's, so off-screen things still cast shadows into view.
- `light-priority`: seats in the shader and shadows had gone to the first lights in component storage, which shuffles when components are removed. Ranking is by brightness at a focus point, `intensity / (d² + r²)` without the range window so far lights do not all tie at zero, with last frame's choices counting 1.25 times (hysteresis) so similar lights do not swap every frame. Seats were raised to 64 point and 8 spotlights. The demo got a field of 42 lamps with pillars (later 100, `firefly-lamps`, Oct 11).
- The light and shadow follow-ups were done in a fixed order, each tested in the game before the next started:
  1. `light-manual-priority`: a `priority` on lights, so a game can mark a boss's aura or a story lamp that must never be dropped in view. Only the ranking function changed.
  2. `light-fading`: handovers fade over 0.25 s instead of popping. It came before caching because it changes how slots are handed over. Lights just switched on or newly in view appear at full strength, since nothing showed them before.
  3. `shadow-face-culling`: a cube face whose view misses the camera's view gets no tile. In the lamp field it mostly skipped the face pointing back at the camera, about 16 of 96 faces, freeing atlas room and draw time for caching.
  4. `shadow-caching`: tiles redrawn only when something in them changes. In the lamp field, standing still, about 49 of 82 tiles were cached; the rest saw the spinning objects, the flashlight or the moon box.
     - `more-point-shadows`: the shadow cap went from 16 to 64 point lights, as many as the seats, since the atlas still fit everything at the smallest tile size. `LightLimits` lets tests cap shadows below the seats to keep shadow-only handovers covered. The shader started skipping lights beyond their range, and their 9 shadow lookups.
     - `sharper-spot-shadows` (Oct 18): spotlight beams land on the ground at a slant well short of their reach, which stretched each texel, and the floodlights' step and pole shadows were jagged. Spotlights may now grow to 2048-pixel tiles and aim for 0.02 units per texel; the floodlights got 2048 and the flashlight 512 (it had 256). They are few and usually still, so their cached tiles cost nothing.
  5. `soft-shadows` (PCSS): a new `emitterRadius` replaced `sourceRadius` for shadows, because `sourceRadius` had been tuned as a falloff softener far bigger than any real bulb (4.5 on the demo flashlight). The first values, 0.1 and 0.05, looked too soft and became 0.05 for point lights and 0.025 for spotlights. Directional lights kept the 3x3 filter then, because a real sun's penumbra was smaller than a texel of the 30-unit box (soft sun shadows came with the cascades in phase 6c).
- `faster-builds`: a one-line change took about 76 s (27 s for the game, 49 s for the tests), because everything compiled in one `g++` call. Objects per file, GCC's `-MMD -MP` header dependencies, parallel builds with output kept in order, and engine objects shared by the game and the tests brought it to about 2 s, with a clean build of everything about 10 s.

## Phase 5: Collision and movement (Oct 9-18 2026)

What both genres need from physics: knowing what you hit, and moving a character through a level without passing through walls.

- `settings-structs`: done before physics added dozens of new constants. Merged on the way: the first-person and free cameras' shared look-and-move steps (`turnCamera`, `moveCamera`) and angle wrapping (`math/angles`). Numbers the shaders share with C++ moved into macro-only headers both include.
- `collision-shapes`: `Aabb` is the old `ModelBounds`, moved to `math/`.
- `raycasts`: every triangle was tested at first; the grids came after.
- `mouse-picking`: two-stage picking, shapes first and triangles to confirm, because games click simple shapes rather than meshes.
- `broad-phase`: the `CollisionScene` is rebuilt from every `Collider` rather than updated, on purpose: incremental updates would need change tracking the ECS does not have, and could leave ghost entries. Tested against brute force on random scenes.
- `static-level`: built as pieces are added and never rebuilt. The demo showed rays slipping through the seam between two triangles, so the triangle test allows a tiny margin (kept as a bit-exact regression test). The demo got its obstacle course, whose collision mesh comes from the same parts as what is drawn, and hover picking where walls hide what is behind them.
- `character-controller`, `first-person-body`: tuned to feel like Overwatch from community-measured numbers (5.5 m/s walking, 90% backwards, 3 m/s crouched, a 5.72 m/s jump, 17.5 m/s² gravity with quadratic drag to a 30 m/s terminal speed, slope-consistent speed and speed-keeping air steering from the 2018 movement update); ground and air acceleration are guesses tuned by feel. The tuning is approved: ask before changing it. Settling onto the ground drops in 0.05 steps because dropping the whole way at once let a kerb's edge win the push-out and shove the capsule sideways. The body replaced the camera-only walking and the placeholder jump (`fps-jump`).
- `floodlights`: four shadow-casting spotlights over the obstacle course.
- `particle-physics`: flight uses the average of the start and end velocities, exact for constant gravity, so throws follow the true parabola; collisions are swept, so nothing passes through a wall at any speed. Particles colliding with each other were left out: piles need phase 14's iterative contact solver. The shared part for cloth and orbits is the per-particle maths; cloth keeps its own array store (phase 7), and the functions become templates over the vector type when phase 13 adds `DVec3`.
- `profiler` (moved here from the UI phase): physics was the first work whose cost grows with the scene, so the release build and every later performance decision should rest on measurements. It counts per call, since ticks do not fall one per frame, and shows each half-second window's average and worst call, so spikes are not averaged away. RENDER included waiting for the GPU then: SDL_GPU could not time the GPU itself.
- `release-build` (moved here from the optional list). Per tick, 40 characters walking a stepped level and 200 bouncing balls: debug 0.52 and 0.54 ms; `-O2` alone 0.105 and 0.090 ms (5-6 times faster); with the vector operators inlined 0.075 and 0.067 ms (7-8 times faster than debug); link-time optimisation 0.073 and 0.061 ms, but a one-file rebuild took 20 s instead of 3 s, so it was left out. The `-O2` build also showed a false array-bounds warning inside `std::sort` in `closestPointsSegmentBox`, now an insertion sort of its at most 8 cuts.
- `collision-guide`: the "Collision and movement" chapter of `docs/ENGINE_GUIDE.md`.

## Phase 6: Rendering foundations, lighting and global illumination (Oct 18 2026 onwards)

### The re-plan (Oct 18 2026)

After a three-way review of the roadmap (a hyper-critical, a neutral and an advantages-focused reviewer, who then answered each other; `roadmap-review`):

- **SDL_GPU is replaced by the engine's own Vulkan 1.3 renderer before global illumination.** SDL_GPU never hands out its Vulkan device, so it could not reach hardware ray tracing, GPU timestamps, bindless resources, GPU-written draw counts or DLSS and FSR, and the old phase 14 plan ("SDL keeps the window while a second backend talks to Vulkan") would have meant every pass written twice. Raw Vulkan with volk, VMA and vk-bootstrap rather than a layer such as NVRHI: full performance and the real concepts, with NVRHI as the fallback if the port stalled past about four weeks (it did not).
- **Work that does not touch the graphics API comes first (6a), renderer work after the port (6c onwards)**, so no renderer code is written twice.
- **DDGI is traced with hardware ray queries first**; a software tracer for GPUs without ray tracing follows as the lower quality setting.
- **Shader maths keeps its tested CPU mirrors**, and a GPU test tier checks the real shaders against them.
- Gaps a modern renderer needs were added: cascaded sun shadows, TAA, reflections, transparency, volumetric fog.

### Before the re-plan

- `hdr-tone-mapping`: it came before the HUD (phase 9) because it changes where everything drawn on top of the scene goes, so the HUD is built once, over the finished image. The dither moved into the tone mapping pass (the float scene image needs none). Exposure defaults to 1.5 so scenes tuned for the old clipped look keep their shadows and mid-tones; AgX is the default curve.
- `hdr-output`: built for the user's HDR monitor; the renderer reads the window's HDR state every frame and swaps the swapchain when it changes.
- `bloom`: tone mapping mixes a share of the blur in place of the same share of the scene, so light is moved, never added.
- `pbr-materials`: the BRDF is multiplied by π so lights keep their old brightness (a white matte surface lit head-on by 1 shows 1), and the old Blinn-Phong default matches roughness 0.5. The metallic-roughness texture was the engine's first non-sRGB texture.

### 6a: groundwork that does not touch the graphics API

- `engine-hygiene`: the GPU device's debug mode had been hardcoded on, which skewed every release measurement. The determinism test came before cloth and ragdolls make the simulation fragile. The deferred create and destroy queue replaced `updateGrenades` doing it by hand.
- `quaternions` (moved here from phase 8): a tumbling body passes through gimbal lock, and blending three angles separately does not give the in-between orientation; ragdolls and glTF skins need them. Earlier was cheaper: 29 uses of Euler angles in 10 files then. Spinners keep Euler angles and convert each tick; characters keep their own yaw, so the turning logic did not change.
- `reversed-z` (moved here from phase 13): two surfaces 1 cm apart at 1 km now get about 270 floats between their depths, where before they got the same depth. Camera-relative rendering: 12 km out, the screen error dropped from about a quarter pixel to about 20,000 times less. The shadow pass, culling and debug lines stayed in world space so cached shadow tiles survive camera movement. The far plane stays finite.
- `debug-ui` (moved here from phase 9): tuning GI, PBR and cloth without sliders is slow. One `visit()` list per settings struct drives the sliders, presets, the saved file and the tests of the defaults. The UI is drawn into its own overlay image and laid over everything last, decoded and scaled to the white level so it looks right in HDR: the path phase 9's HUD will take. The Low and Medium presets were first guesses until 6b's GPU timings.
- `normal-maps` (moved here from phase 10): every glTF asset uses them, and PBR looks flat without them. MikkTSpace, because it is the standard every baking tool uses, so maps look as their authors made them; checked against the tangents Blender exported for NormalTangentMirrorTest. The demo got a gallery of three Khronos test models.

### 6b: the Vulkan 1.3 renderer (Oct 18-19 2026)

- `golden-images` first, on SDL_GPU, so the port could be checked bit for bit. The planned furnace sphere became the gold teapot and the gallery, since the night scene cannot light anything evenly. All twelve images took about 6.5 MB; two runs, and the debug and release builds, matched bit for bit.
- `vulkan-backend`: built beside `GpuRenderer` behind a temporary interface, running the same compiled shaders in SDL_GPU's descriptor layout, until all twelve golden images matched SDL_GPU's bit for bit in the debug and release builds with the validation layers silent. It was then tried live (debug UI, HDR on and off, resizing, minimising, present modes), and SDL_GPU, the interface and ImGui's SDL_GPU backend were removed. Barriers are written by hand per pass; a render graph waits until the passes outgrow that.
- `gpu-timestamps` (the renderer timing breakdown, moved here from phase 14), so every later change is judged by its own number. First numbers on the RTX 5080 at the start view: 0.20 ms for the whole frame (main pass 0.11, bloom 0.04, shadows 0.03, tone mapping 0.02). Debug labels stay on in release builds, since they cost nothing without a tool attached.
- `frames-in-flight`: two by default, one as a low-latency option, switchable live. At 0.2 ms of GPU work per frame the two modes measured the same; two pays off once the GPU's work grows towards the CPU's.
- `gpu-test-tier`: the shaders' maths moved into eight libraries of pure functions with the golden images unchanged bit for bit; about 20,000 values in 32 groups compared with the C++ mirrors. First results on the RTX 5080: every group within 16% of its tolerance except the noise (40%); planted bugs failed at 4,000 to 27,000 times the tolerance. Also fixed: the shaders' dependency files had lived in a folder per build, which let the other build keep stale shaders; they now share one. Not done: the optional Slang trial (phase 14).

### 6c: GPU scene, compute and lights (Oct 19 2026)

- `gpu-scene`, in three steps:
  1. The same per-object data moved into storage buffers, golden images unchanged bit for bit. Lamp field: 161 visible objects in 10 draw calls; CPU recording went from 1.75 to about 1.15 ms with every shadow tile redrawn.
  2. Shadow casters culled by each light's range sphere before its faces (from phase 14's performance list): 1861 caster instances instead of 2299. 6 pixels of one golden image changed, where a soft shadow's wide filter reached past a light's range.
  3. One world-space instance per object shared by every view: about 61 KB uploaded per frame instead of 155. The golden references were saved again for rounding (scattered soft shadow samples flipping; at most 6 steps of 255 in the final images).
  Bounds are not in the GPU records yet: nothing on the GPU reads them before GPU culling.
- `depth-prepass`: the plan was to reuse the shadow pass's depth-only pipeline, but it multiplies world matrices and the main pass camera-relative ones, which round differently, and the `EQUAL` test needs identical bits. It uses the main pass's own vertex shader with `gl_Position` declared `invariant` instead; `invariant` alone changed 10 references by rounding (single edge pixels). Measured on the RTX 5080, main pass plus prepass at 2560x1440: floodlights 0.39 to 0.32 ms, start and gallery 3-4% less, the lamp field unchanged at 1.2 ms (little overdraw there; its cost was every pixel looping over 64 lights, which clustered shading removed). The prepass itself costs 0.005-0.017 ms.
- Compute pipelines: built with clustered shading, on the GPU test tier's `ComputePipeline`. Storage images (writing textures from compute) wait for their first user, DDGI's probe atlas. Shaders now compile for Vulkan 1.3.
- `clustered-shading` (moved here from phase 14): each pixel walks its cluster's bits lowest first, which is slot order, so all twelve golden images were identical bit for bit with clustering on and off. Then every light the buffers hold was seated by default (new lamp-field references, its back rows now lit). Measured on the RTX 5080 at 2560x1440, main pass: lamp field 1.18 to 0.75 ms with 64 lamps (0.62 to 0.30 without point shadows; the rest is the soft shadows of lamps that really reach), 0.78 ms with all 88 in view (1.40 the old way), start and gallery about 11-13% less, floodlights unchanged; the cluster pass 0.004-0.008 ms. The heatmap shows far clusters listing 16 or more lamps where 4-8 reach; tighter clusters are the next step if many lights ever cost more than a distance check each (phase 14).
- `lightlab`: a second game for judging cascades and DDGI, since the demo is a 40 m night scene with no inside; the demo stays as the regression scene. Moving the demo's camera, modes and body into `PlayerControls` left its golden images and determinism unchanged bit for bit. Its golden views are the "before" picture: sun shadows ended about 30 m out, and the room was lit only by its beam and the flat ambient.
- `cascaded-shadows`: judged in the light lab, which drove three rounds of changes:
  - Splits: λ 0.75 left the first cascade 13 m deep with 3.6 cm texels; 0.95 gives about 3, 9, 36 and 200 m.
  - Tiles: 1024 left medium distances at 10 cm per texel, so every cascade became 2048, and then a new Ultra preset, made the default, with 4096 cascades in an atlas grown to 384 MB (High keeps 2048 in the usual 128 MB). Texels at 4096 for a 70° view: 0.22 cm near the camera, 0.65 cm to 9 m, 2.5 cm to 36 m, 14 cm to 200 m.
  - Bias: a normal offset moves the point sideways by an amount growing with the texel, so a building's shadow edge landed in different places in neighbouring cascades (a visible step under a low sun). Moving the point towards the sun instead fixed it, and also the stair-stepped window patch in the sunlit room.
  Only the brightest directional light is shadowed now; its 4 old tiles became the cascades. All 18 golden references were saved again (shadows now reach 200 m, the moon's are crisper).
- `vram-readout`: asked for once the Ultra preset's 384 MB atlas made memory a choice.
- `sun-disc`: the disc's brightness is capped at 30,000 by scaling the whole colour, because full sun would overflow the half-float scene image about 4.6 times.
