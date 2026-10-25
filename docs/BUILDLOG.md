# Build log

The history of the finished roadmap items: when each was built, why, what was measured at the time, and what was tried and dropped.

It is kept apart from the plan so `ROADMAP.md` stays a short list of what is done and what comes next.

- `ROADMAP.md`: the plan; finished items there are one line each.
- `docs/ARCHITECTURE.md`: how the code works today (names, files, numbers). Facts about the current code belong there, not here.
- This file: decisions, reasons, measurements and dead ends, in the order they happened, grouped by roadmap phase.
  - Numbers are as measured then, so later work may have changed them.
  - Branch names are given so each step can be found in `git log` (`git log --merges` lists them with dates).

When a roadmap item is ticked off, cut it to one line there and add its story here, in the same branch.

---

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

## Phase 6: Rendering foundations, lighting and global illumination (Oct 18-24 2026)

### The re-plan (Oct 18 2026)

After a three-way review of the roadmap (a hyper-critical, a neutral and an advantages-focused reviewer, who then answered each other; `roadmap-review`):

- **SDL_GPU is replaced by the engine's own Vulkan 1.3 renderer before global illumination.**
  - SDL_GPU never hands out its Vulkan device, so it could not reach hardware ray tracing, GPU timestamps, bindless resources, GPU-written draw counts or DLSS and FSR.
  - The old phase 14 plan ("SDL keeps the window while a second backend talks to Vulkan") would have meant every pass written twice.
  - Raw Vulkan with volk, VMA and vk-bootstrap rather than a layer such as NVRHI: full performance and the real concepts, with NVRHI as the fallback if the port stalled past about four weeks (it did not).
- **Work that does not touch the graphics API comes first (6a), renderer work after the port (6c onwards)**, so no renderer code is written twice.
- **DDGI is traced with hardware ray queries first**; a software tracer for GPUs without ray tracing follows as the lower quality setting.
- **Shader maths keeps its tested CPU mirrors**, and a GPU test tier checks the real shaders against them.
- Gaps a modern renderer needs were added: cascaded sun shadows, TAA, reflections, transparency, volumetric fog.

### Before the re-plan

- `hdr-tone-mapping`: it came before the HUD (phase 9) because it changes where everything drawn on top of the scene goes, so the HUD is built once, over the finished image.
  - The dither moved into the tone mapping pass (the float scene image needs none).
  - Exposure defaults to 1.5 so scenes tuned for the old clipped look keep their shadows and mid-tones; AgX is the default curve.
- `hdr-output`: built for the user's HDR monitor; the renderer reads the window's HDR state every frame and swaps the swapchain when it changes.
- `bloom`: tone mapping mixes a share of the blur in place of the same share of the scene, so light is moved, never added.
- `pbr-materials`:
  - The BRDF is multiplied by π so lights keep their old brightness (a white matte surface lit head-on by 1 shows 1), and the old Blinn-Phong default matches roughness 0.5.
  - The metallic-roughness texture was the engine's first non-sRGB texture.

### 6a: groundwork that does not touch the graphics API

- `engine-hygiene`:
  - The GPU device's debug mode had been hardcoded on, which skewed every release measurement.
  - The determinism test came before cloth and ragdolls make the simulation fragile.
  - The deferred create and destroy queue replaced `updateGrenades` doing it by hand.
- `quaternions` (moved here from phase 8): a tumbling body passes through gimbal lock, and blending three angles separately does not give the in-between orientation; ragdolls and glTF skins need them.
  - Earlier was cheaper: 29 uses of Euler angles in 10 files then.
  - Spinners keep Euler angles and convert each tick; characters keep their own yaw, so the turning logic did not change.
- `reversed-z` (moved here from phase 13): two surfaces 1 cm apart at 1 km now get about 270 floats between their depths, where before they got the same depth.
  - Camera-relative rendering: 12 km out, the screen error dropped from about a quarter pixel to about 20,000 times less.
  - The shadow pass, culling and debug lines stayed in world space so cached shadow tiles survive camera movement.
  - The far plane stays finite.
- `debug-ui` (moved here from phase 9): tuning GI, PBR and cloth without sliders is slow.
  - One `visit()` list per settings struct drives the sliders, presets, the saved file and the tests of the defaults.
  - The UI is drawn into its own overlay image and laid over everything last, decoded and scaled to the white level so it looks right in HDR: the path phase 9's HUD will take.
  - The Low and Medium presets were first guesses until 6b's GPU timings.
- `normal-maps` (moved here from phase 10): every glTF asset uses them, and PBR looks flat without them.
  - MikkTSpace, because it is the standard every baking tool uses, so maps look as their authors made them; checked against the tangents Blender exported for NormalTangentMirrorTest.
  - The demo got a gallery of three Khronos test models.

### 6b: the Vulkan 1.3 renderer (Oct 18-19 2026)

- `golden-images` first, on SDL_GPU, so the port could be checked bit for bit.
  - The planned furnace sphere became the gold teapot and the gallery, since the night scene cannot light anything evenly.
  - All twelve images took about 6.5 MB; two runs, and the debug and release builds, matched bit for bit.
- `vulkan-backend`: built beside `GpuRenderer` behind a temporary interface, running the same compiled shaders in SDL_GPU's descriptor layout, until all twelve golden images matched SDL_GPU's bit for bit in the debug and release builds with the validation layers silent.
  - It was then tried live (debug UI, HDR on and off, resizing, minimising, present modes), and SDL_GPU, the interface and ImGui's SDL_GPU backend were removed.
  - Barriers are written by hand per pass; a render graph waits until the passes outgrow that.
- `gpu-timestamps` (the renderer timing breakdown, moved here from phase 14), so every later change is judged by its own number.
  - First numbers on the RTX 5080 at the start view: 0.20 ms for the whole frame (main pass 0.11, bloom 0.04, shadows 0.03, tone mapping 0.02).
  - Debug labels stay on in release builds, since they cost nothing without a tool attached.
- `frames-in-flight`: two by default, one as a low-latency option, switchable live. At 0.2 ms of GPU work per frame the two modes measured the same; two pays off once the GPU's work grows towards the CPU's.
- `gpu-test-tier`: the shaders' maths moved into eight libraries of pure functions with the golden images unchanged bit for bit; about 20,000 values in 32 groups compared with the C++ mirrors.
  - First results on the RTX 5080: every group within 16% of its tolerance except the noise (40%); planted bugs failed at 4,000 to 27,000 times the tolerance.
  - Also fixed: the shaders' dependency files had lived in a folder per build, which let the other build keep stale shaders; they now share one.
  - Not done: the optional Slang trial (phase 14).

### 6c: GPU scene, compute and lights (Oct 19 2026)

- `gpu-scene`, in three steps:
  1. The same per-object data moved into storage buffers, golden images unchanged bit for bit. Lamp field: 161 visible objects in 10 draw calls; CPU recording went from 1.75 to about 1.15 ms with every shadow tile redrawn.
  2. Shadow casters culled by each light's range sphere before its faces (from phase 14's performance list): 1861 caster instances instead of 2299. 6 pixels of one golden image changed, where a soft shadow's wide filter reached past a light's range.
  3. One world-space instance per object shared by every view: about 61 KB uploaded per frame instead of 155. The golden references were saved again for rounding (scattered soft shadow samples flipping; at most 6 steps of 255 in the final images).

  Bounds are not in the GPU records yet: nothing on the GPU reads them before GPU culling.
- `depth-prepass`:
  - **Plan and change:** the plan was to reuse the shadow pass's depth-only pipeline, but it multiplies world matrices and the main pass camera-relative ones, which round differently, and the `EQUAL` test needs identical bits. It uses the main pass's own vertex shader with `gl_Position` declared `invariant` instead; `invariant` alone changed 10 references by rounding (single edge pixels).
  - **Measured** on the RTX 5080, main pass plus prepass at 2560x1440:
    - floodlights 0.39 to 0.32 ms
    - start and gallery 3-4% less
    - the lamp field unchanged at 1.2 ms (little overdraw there; its cost was every pixel looping over 64 lights, which clustered shading removed)
    - the prepass itself costs 0.005-0.017 ms.
- Compute pipelines: built with clustered shading, on the GPU test tier's `ComputePipeline`.
  - Storage images (writing textures from compute) wait for their first user, DDGI's probe atlas.
  - Shaders now compile for Vulkan 1.3.
- `clustered-shading` (moved here from phase 14): each pixel walks its cluster's bits lowest first, which is slot order, so all twelve golden images were identical bit for bit with clustering on and off.
  - Then every light the buffers hold was seated by default (new lamp-field references, its back rows now lit).
  - **Measured** on the RTX 5080 at 2560x1440, main pass:
    - lamp field 1.18 to 0.75 ms with 64 lamps (0.62 to 0.30 without point shadows; the rest is the soft shadows of lamps that really reach)
    - 0.78 ms with all 88 in view (1.40 the old way)
    - start and gallery about 11-13% less, floodlights unchanged
    - the cluster pass 0.004-0.008 ms.
  - The heatmap shows far clusters listing 16 or more lamps where 4-8 reach; tighter clusters are the next step if many lights ever cost more than a distance check each (phase 14).
- `lightlab`: a second game for judging cascades and DDGI, since the demo is a 40 m night scene with no inside; the demo stays as the regression scene.
  - Moving the demo's camera, modes and body into `PlayerControls` left its golden images and determinism unchanged bit for bit.
  - Its golden views are the "before" picture: sun shadows ended about 30 m out, and the room was lit only by its beam and the flat ambient.
- `cascaded-shadows`: judged in the light lab, which drove three rounds of changes:
  - **Splits:** λ 0.75 left the first cascade 13 m deep with 3.6 cm texels; 0.95 gives about 3, 9, 36 and 200 m.
  - **Tiles:** 1024 left medium distances at 10 cm per texel, so every cascade became 2048.
    - Then a new Ultra preset, made the default, with 4096 cascades in an atlas grown to 384 MB (High keeps 2048 in the usual 128 MB).
    - Texels at 4096 for a 70° view: 0.22 cm near the camera, 0.65 cm to 9 m, 2.5 cm to 36 m, 14 cm to 200 m.
  - **Bias:** a normal offset moves the point sideways by an amount growing with the texel, so a building's shadow edge landed in different places in neighbouring cascades (a visible step under a low sun). Moving the point towards the sun instead fixed it, and also the stair-stepped window patch in the sunlit room.

  Only the brightest directional light is shadowed now; its 4 old tiles became the cascades. All 18 golden references were saved again (shadows now reach 200 m, the moon's are crisper).
- `vram-readout`: asked for once the Ultra preset's 384 MB atlas made memory a choice.
- `sun-disc`: the disc's brightness is capped at 30,000 by scaling the whole colour, because full sun would overflow the half-float scene image about 4.6 times.
- `sdsm` (sample distribution shadow maps, Lauritzen 2011): the cascades end at the farthest surface on screen instead of always at 200 m. Decisions:
  - **Only the far end is fitted.** Starting the splits at the nearest surface (about 1.5 m when standing in the open) pushes every logarithmic split outwards, so the pixels nearest the camera land in a bigger, coarser box than before.
  - **The measurement is used a frame or two late**, read back from the frame that drew it, rather than building the cascade matrices on the GPU.
    - That keeps the CPU's per-cascade culling and the shadow cache; GPU-built cascades would need GPU culling (phase 14).
    - The price is that a sudden reveal (turning from a wall to the horizon) can leave the newly seen far ground unshadowed for a frame or two.
  - **A ladder of distances:** the fitted distance moves on a ladder of steps 1.25 apart, after a 10% margin, climbing at once and coming down only two steps later. A distance following every small change would resize every texel every frame (shimmer) and redraw the cascades every frame.
  - **The reduction** keeps depths as their float bits, which sort like the numbers for positive floats, so the GPU combines them with integer atomics. The range lives in GPU memory and is copied to read-back memory, rather than having every workgroup's atomics cross the bus.
  - **The GPU test** runs the real pass on uploaded depth images. Its first version planted a denormal, which the D32 image flushed to zero (sky); real depths never come near one, so the case uses the smallest normal float.
  - **A mistake along the way:** adding `sampledImages` in the middle of `ComputePipelineDescription` broke the GPU test runner, which fills the struct positionally (every library test read 0). The field went after `pushConstantBytes`.
  - **Golden captures** are a cold first frame, with no earlier frame to have measured. They now draw the frame once to measure it.
    - With the measurement ignored, that extra frame alone still changed the lamp field (a warm light history moved shadows between lamps).
    - So it now restores the light history, atlas layout and shadow cache afterwards; with the measurement ignored, all 22 references then matched bit for bit.

  Results on the RTX 5080 (sdsm):

  - The sunlit room fits to 11.6 m, and its cascades' texels went from 0.22, 0.65, 2.5 and 14 cm to 0.032, 0.092, 0.27 and 0.81 cm (7 to 17 times finer).
  - Views reaching the horizon (towers, overview, sunset, sun disc, moon) are unchanged, since surfaces reach past 200 m; the demo's close views changed, since its moonlight is a shadowed sun too.
  - 12 references saved again (10 demo, `sunlit-room`), differing only along sun shadow edges.
  - The pass costs 0.015-0.04 ms at 2560x1440.
  - The main pass grew by 0.06 ms in the demo's start view and 0.09 ms in the sunlit room (+8-18%): the window patch's penumbra (about 1 cm) used to be under one 2.5 cm texel and now spans about four of 0.27 cm, so the soft shadow's samples spread wider in the atlas; other views were within 2%.

### 6d: global illumination (Oct 19 2026 onwards)

#### Units, ray queries and the first probes

- Units re-derived for DDGI against physically based shading (in `docs/ILLUMINATION.md`):
  - The old claim that a probe's value goes straight into the ambient slot still holds, for a new reason (pixel values are π times radiance, and ambient is the sky's irradiance), but only as diffuse light, so the furnace test now expects B × (1 − specular albedo).
  - It also showed the light lab's sky ambient dial (0.2) is about 4 times below what its drawn sky (about 0.75) would give, to retune once DDGI lights it.
- `ray-queries`: hardware ray tracing's foundation. Decisions:
  - **Vertex data through buffer device addresses** (a per-mesh table), not one merged vertex buffer: meshes stay as they were, and ray tracing needs device addresses anyway. Addresses travel as two 32-bit halves (`GL_EXT_buffer_reference_uvec2`); the first version used 64-bit integers, which need the optional `shaderInt64` feature, caught by the validation layers.
  - **The top-level structure is rebuilt from scratch every frame** rather than refitted: simple and never stale, and measured at 0.05-0.11 ms for 29-900 instances.
  - **Every drawn object goes in, with a mask:** shadow casters solid, the rest visual-only.
    - The first plan was shadow casters only. The user asked whether anything else would ever be wanted, and three cases came up: glowing things that are not lights should light their surroundings, reflections will need the sun disc and bulbs, and a game might switch shadows off on cheap clutter for speed.
    - Masks keep all of them open at no cost; GI rays see only solid ones, so the glowing faces of lights already counted are not counted twice.
  - **Which side is the front** was left for the GPU test to settle: with no instance flags the hardware's front is the engine's anticlockwise winding, mirrored instances included, matching the CPU on every ray.
  - **The test** compares 2,000 rays with the CPU's triangle raycast.
    - Its first scene put a cube's bottom exactly on the floor, and five rays hit the coplanar pair at the same distance with the two sides choosing differently; the floor moved down.
    - One grazing hit then differed by 1.4e-5 of its distance. The tolerance now grows with 1/cosine between ray and surface (rounding of where a plane sits moves a grazing hit along the ray), after which the worst hit used 3% of it.
  - `triangle.frag`'s material and texture declarations moved into `materials.glsl` for the debug shader to share; the golden images stayed bit for bit the same.

  Measured on the RTX 5080 at 2560x1440: one primary ray per pixel (3.7 million, coherent) in 0.09-0.28 ms.
- `ddgi-probes`: the probe grid, its rays and update, and a debug view; nothing samples the probes yet, so the picture is unchanged (every golden image identical). Decisions:
  - **Direct light at probe-ray hits comes from shadow rays, not the shadow atlas.**
    - The atlas only holds the lights and cube faces the camera needs, and since sample distribution shadow maps the sun's cascades end at the farthest surface on screen. So a probe ray hitting a wall behind the camera would have no sun shadow at all, and sunlight would pour into closed rooms off screen.
    - Reusing the atlas was rejected as a bug, not a quality trade-off.
    - This branch lights hits by the sun only (plus the sky for misses), which is the light lab's whole lighting; point and spot lights come next, capped to the brightest few per hit by a setting.
  - **Each game places its probe volume** (a `ProbeVolume` component), rather than one fitted to the level's bounds (the light lab runs to the horizon: millions of probes at 1 m, or a grid too coarse for a room) or a window following the camera (the right answer for large maps, but it adds scrolling planes and blending before the basics are proven).
    - The indexing is a single function, so a per-axis scroll offset can be added later.
    - The light lab's 12 x 5 x 16 probes sit on half-metre marks so every wall falls between layers, which stands in for probe relocation for now.
  - **Hysteresis is set as time constants** (0.5 s for irradiance, 1 s for distances; the papers give 0.98 a frame at about 60 fps, 0.83 s, and no values for 2021's split), converted each frame with exp(-frameSeconds / tau).
  - **Borders:** the update copies each square's border from shared memory at the end of its own workgroup, instead of 2021's separate border pass: one dispatch fewer and no barrier between them.
  - **Infinite bounces** (last frame's probes read at each hit) wait for the sampling function, which `triangle.frag` and the trace will share, so it is written and tested once.
  - **The GPU test** runs the real passes on three scenes the CPU works out ray by ray (furnace, sealed box, sunlit floor).
    - The floor first read zero light: the bindless texture set was visible to fragment shaders only, which the validation layers also reported. It is now visible to compute too, and textures end their upload in a new `SurfaceRead` state covering both.
    - It then read 0.4% low, one whole half-float step: Vulkan lets a GPU round toward zero when it stores 16-bit floats, so the tolerance allows a full step (fifth root times five, 6e-3) rather than half of one.
    - The ball shader's `discard` needed `shaderDemoteToHelperInvocation` (required by Vulkan 1.3, turned on), and the two-channel distance atlas `shaderStorageImageExtendedFormats`.
  - **The debug balls** first faced the camera's forward axis and came out cut off near the edges of the view, where a ball's outline stretches away from the middle; each ball's square now faces the line from the camera to it.

  Not measured yet: the probe passes' GPU time (F4 shows it as PROBES); 960 probes at 64 rays are about 61,000 rays plus as many shadow rays a frame, against the 3.7 million primary rays measured at 0.09-0.28 ms.

#### Sampling the probes, and the flicker hunt

- `ddgi-sampling`: the probes light the picture. Surfaces sample them in `triangle.frag`, probe rays' hits read last frame's probes (one more bounce a frame) and add the brightest point and spot lights by shadow rays. Decisions:
  - **The 2021 reference code's weights throughout**, blended in square-root space:
    - wrap back-face weight
    - cubed Chebyshev visibility with its 0.05 floor
    - the crush below 0.2
    - trilinear factors at least 0.001
    - the self-shadow bias of (0.2 n + 0.8 v) × 0.75 × the smallest spacing × 0.3, the cell found from the biased point.

    Visibility and back-face weights use the geometric normal, steadier than the normal map's; the irradiance is read along the shaded normal.
  - **The volume's edge:** the 2021 paper fades a volume out across its outermost cell, inside the probes. That would have left the light lab's floor, half a cell below the lowest layer, on the flat ambient. So a volume here lights points out to half a cell beyond its outer probes at full weight and fades over the next half cell (`DDGI_VOLUME_MARGIN`, `DDGI_VOLUME_FADE`), the flat ambient taking over beyond.
  - **Lights at hits:** the CPU lists the point and spot lights whose range reaches the probes' box grown by the longest recorded distance (1.5 cell diagonals).
    - Each hit ranks those by unshadowed luminance, keeps the brightest `lightsPerHit` (4, at most 8; ties keep the earlier light, so rounding cannot reorder them) and sends a shadow ray to each.
    - A light farther out still lights the screen; only its light on far surfaces the probes see is missed.
  - **Bounce energy 0.85** (the 2019 code's) as a setting. 1 would be physically right, since albedo below 1 already makes the series converge, but the faint leaks it would amplify are what the eye sees first in dark rooms.
  - **Specular from the probes:** the plan kept the specular part on the flat ambient until reflections.
    - With the light lab's ambient raised to match its sky (below), the varnished floor of the sunlit room reflected a bright blue sky it cannot see and turned blue-grey.
    - The specular part now reads the same probes, with the same weights, in the mirror direction: a reflection as blurred as a fully rough one (the 2021 paper's "reflections from probes"), right in brightness indoors and out, for 8 more texture reads.
    - Without probes it is still the flat ambient.
  - **Pictures without probes stay bit for bit the same.**
    - With the probe code behind an ordinary branch, the demo's views moved by a half-float step at about 25 pixels each (one HDR view beyond tolerance): the driver compiled the rest of the shader a little differently.
    - Sampling is now a specialization constant (`DDGI_SAMPLE_PROBES_CONSTANT`), with two more main-pass pipelines, so the pipelines without probes hold none of the code; every demo reference matched exactly again.
    - Graphics pipelines gained `fragmentConstants` for it.
  - **The scene set holds the atlases now** (a one-texel stand-in when there are none), so `VulkanDdgi::prepare` creates them before the set is written each frame. They rest as `SurfaceRead` between updates: sampled by the main pass, the debug balls and the next frame's trace.
  - **Golden images** are one frame from cold, so a view with probes is drawn 240 times first (2 s at a tick each: four irradiance time constants), then captured. The whole light lab check takes 9 s, and a second run reproduces the references exactly.
  - **Light lab:** the sky ambient dial is gone; the flat ambient is the drawn sky's value (a uniform sky drawn at B gives a surface facing it the irradiance B), as the probes see it.
    - Outdoor shadows are lighter, filled by the sky; the sunlit room is darker than before and lit only by the beam, its bounce and the sky through the window.
    - All 10 light lab references saved again for it; the 12 demo references are unchanged.
  - **GPU tests:** the new library's functions against their mirrors, plus a lamp-lit floor pass test (two lamps, one kept per hit).
    - The visibility's variance is the mean square less the mean's square, which cancels when the spread is small, so its tolerance grows with mean square / variance.
    - A first version of the brightest-lights test shader read zeros for lights 2 and 3 (a vector indexed by a loop counter inside `?:`); spelled out as an array, it matched exactly.
  - **Flicker in the dark rooms**, reported on first look, led to dropping 2021's fifth-root storage and per-texel speed-ups for plain linear blending (details and numbers in `docs/ILLUMINATION.md`, Hysteresis).
    - A dark-room texel seeing a small sunlit patch settled at 51% of its true light and moved 8.7% in 0.1 s; linear, within 0.1% and 1.5% (`ddgi_test`).
    - On screen, though, the brightness was fixed but the crawl barely moved: the sunlit room's 90th-percentile change over 0.2 s, 5.8% before and 5.6% after.
    - Measured by changing one default at a time and comparing captures 24 frames apart (a scratch tool, relative luminance change per pixel): 256 rays halved it, holding the distances still changed nothing, no bounces made it worse (a darker room, the same noise).
    - With 8 s to settle, 64 rays gave 3.7% at a 0.5 s time constant, 2.2% at 1 s, 1.4% at 2 s, and 128 rays at 1 s 1.5%.
    - So what is left is Monte Carlo noise, 1/sqrt(rays x time constant); the defaults (64 rays, 0.5 s) were left for the user to choose.
    - Stored irradiance is now checked three times tighter in the GPU test (2e-3), since no fifth power magnifies the half-float rounding.
  - **The flicker hunt went on in the dark back room itself** (the first measurements were of the sunlit room, which hides it): 6.4% of crawl over 0.2 s at the defaults, against 3.7% next door, and 2.1% even at 256 rays.
    - A preview showed that the rays' own luck is most of it: frames taking turns through one long spiral (16 frames a cycle, `DDGI_RAY_CYCLE`, one random rotation per cycle) left 3.8% at 64 rays and 1.3% at 256, for nothing.
    - 8 and 64 frames a cycle did slightly worse (4.3%, 4.4%; single measurements, good to a few tenths).
    - The user tried it live with the rays and time constant sliders and chose interleaving with a 2 s irradiance time constant ("eliminates it completely"), keeping 64 rays: 1.2% in the back room, 0.66% at 256 rays.
    - The price, told to the user, is lag: bounce light follows a change over a few seconds, and moving lights and objects trail theirs, until scene events exist.
    - Golden views with probes now warm up for 960 frames (8 s, four time constants); the GPU tier takes 33 s.
    - The CPU sparse-texel test crawls 2.4% with independent rotations, 1.3% interleaved and 0.33% at 2 s.
  - **The light lab got the demo's flashlight** (F, off at the start, so its golden images are unchanged), to watch how a moving light's bounce trails behind it with the 2 s time constant.
    - Its logic moved from `DemoGame` into a shared `gameplay/flashlight` (`Flashlight`, `flashlightInHand`, `flashlightBeam`) rather than being copied; the demo's golden images are unchanged too.
    - The ways to fix trailing for fast lights, explained to the user: two irradiance atlases split by how fast their light's source moves (fast lights at a short time constant, no extra rays; recommended), faster probes near moving lights, reflective shadow maps for spotlights' first bounce, or screen-space GI.
  - **A dense-probe test for the light lab** (`lightlab-dense-probes`): the user asked for probes 20 cm apart.
    - The whole volume at that spacing would be 89,376 probes, past both the 65,535-probe limit (one workgroup per probe, along one dispatch axis) and the 16,384-texel atlas width.
    - So the dense grid covers the building only: 27 x 15 x 47 = 19,035 probes, about 20 times the probe work, a setting off by default (golden images unchanged).
    - Rendered once with the validation layers on: clean, and the bounce light visibly sharper (corners and wall feet darken, the beam's glow beside the doorway tighter).
    - Per-probe noise is unchanged (still 64 rays each), only finer-grained.

  Not measured yet: the main pass's added cost of 8 corners, 24 texture reads a pixel, and the trace's added sampling and shadow rays; F4 shows both (MAIN and PROBES).

#### Fast lights, presets and the ray budget

- `ddgi-fast-lights`: a flashlight's bounce trailed behind it for seconds, because the probes' memory is 2 s, kept that long against flicker.
  - **The split:** light adds up, so it is split. Lights flagged fast (`PointLight::fast`, `SpotLight::fast`; the flashlight is) have their bounce kept in a second irradiance atlas with a 0.05 s memory, summed with the still one wherever the probes are read.
  - **Bounces keep their source's memory:** the trace keeps each ray's light from still and fast lights apart, including the bounce read from last frame's probes, still into still and fast into fast, so a fast light's later bounces keep its short memory too.
  - **More rays near fast lights:** probes within reach of a fast light (its range plus the probes' longest distance, as the hit lights are chosen) trace 256 rays, the rest 64.
  - **Hitches:** the fast blend counts a frame as at most 1/30 s, so a hitch cannot replace its history with one noisy frame.

  Decisions:
  - **Each atlas is blended on its own** with the same corner weights and the two added, rather than adding the probes' values first: a probe ray's hit must pass each on into its own atlas anyway, and with the fast atlas empty the picture is exactly the old one. Every golden image stayed bit for bit the same; a new light lab view, `flashlight` (the dark back room lit by the flashlight), puts the fast atlas into the golden images.
  - **The fast light's part of each ray** is stored as half floats in a buffer of its own (8 bytes a ray), so the still rays stay full floats (the pictures without fast lights unchanged) and the ray memory grows by half rather than doubling (the dense probe test's 19,035 probes x 256 rays: 39 MB more, not 78).
  - **Which probes trace more rays is decided on the GPU:** the trace checks its probe against the fast hit lights and writes a flag the update reads. The dispatch makes room for 256 rays a probe only when a fast light is listed, so frames without one dispatch exactly as before.
  - **Tried and dropped: a ray cycle fitted to each light's memory.**
    - The plan: a power of two with about three whole cycles in the time constant's frames (from a back-room measurement at 0.5 s and 120 fps, where 8 and 16 frames a cycle were about equally good and 64 worse), from a smoothed frame rate, with hysteresis against jitter, under the still cycle's rotation with the phase taken modulo the shorter cycle.
    - Built and measured, it lost. The 90th percentile of brightness change in 0.2 s in the back room, fast part at 0.05 s and 256 rays:

      | Frame rate | The rule's cycle | The long cycle |
      |---|---|---|
      | 240 fps | 0.69% (4 frames) | 0.55% |
      | 120 fps | 1.36% | 1.22% |
      | 60 fps | 2.59% | 1.60% |

      Other cycles at 240 fps: 8 frames 0.52%, 2: 1.16%, 1: 1.89%.
    - Why: under the shared rotation, which turns only every 16 frames, a short cycle traces the same few ray sets again and again until it turns (at 60 fps, the rule's cycle of 1 traced the identical 256 rays for 16 frames), and repeated directions add nothing.
    - The cycle fitting, the frame-rate smoothing, the rays' clock and the cycles-per-window setting were removed: every probe takes turns through the same 16-frame cycle, and fast probes simply trace more rays. Fitting cycles each with its own rotation was not tried.
  - **The fast memory stays 0.05 s** (the user's choice, keeping up with the flashlight best). Flicker in the back room:
    - at 240 fps: 0.55% at 0.05 s, 0.32% at 0.1 s, 0.22% at 0.2 s, against 0.25% for the still light alone (the fast part at 2 s)
    - at 60 fps: 1.6% at 0.05 s and 0.47% at 0.2 s
    - 64 fast rays flickered 2.0% at 240 fps.
  - **Measured with two scratch tools:**
    - Captures of the `flashlight` view N and N + k frames apart, compared as relative luminance change per pixel. `Application::captureGoldenView` gained the warm-up frame count for it: the probes are deterministic, so two runs warmed up for N and N + k frames are frames N and N + k of one run.
    - The renderer's GPU timestamps for the same view at 1280 x 720 on the RTX 5080, against the same view built from `main`.

    Results:
    - PROBES went from 0.032 ms to 0.106 ms with the flashlight on (every probe of the light lab's volume is within its 50 m reach, so all trace 256 rays), and from 0.031 to 0.036 ms with it off (the fast atlas's update and the reach test).
    - MAIN from 0.275 to 0.287 ms (the fast atlas's 8 more texture reads, 16 with the specular part).
    - With the dense probe test and the flashlight on, PROBES went from 0.58 ms to 2.84 ms (0.76 ms with 64 fast rays): the price of 256 rays for every probe in reach.
- `ultra-gi-defaults`, then `sun-clock-slower` (Oct 20 2026):
  - The light lab's sun clock runs by default, 0.025 h/s (a day in 16 minutes; was 0), and golden views stop it (`prepareGoldenView`), so their 0.5 s of ticks leave the sun at its hour.
  - The first branch also made 256 rays a probe and a 0.5 s irradiance time constant the defaults (the same rays times time constant as 64 at 2 s, so the same noise at a quarter of the lag, for four times the rays).
  - The user then went back to 64 rays and 2 s, so the golden images are the ones from before.
- `ddgi-ray-budget` (Oct 20 2026): the roadmap's optional cap on probe updates a second, built as a ray budget instead of probes taking turns.
  - **The two were weighed:**
    - Probes updating in turns (16 interleaved groups, each blending with its own time since its last update) would have needed a ray cycle per group (a probe updated every 4th frame of a global 16-frame cycle sees only 4 of its 16 phases) and a special case for probes near fast lights, whose 0.05 s memory cannot wait.
    - Tracing fewer rays a frame keeps every probe updating every frame with today's blend, for at most a 4x saving (16 rays is the least) and the per-probe fixed costs kept.
    - The user took the simpler one.
  - **Measured before building:** `ddgi_test`'s dark-room texel model (a 15 degree bright patch, tau 2 s, 200 s per run, two seeds), the crawl over 0.1 s:

    | Rays and frame rate | 15 degree patch | 8 degree patch |
    |---|---|---|
    | 64 rays at 60 fps | 0.94-0.96% | 2.0% |
    | 64 at 240 fps | 0.32-0.33% | 0.72% |
    | 16 at 240 fps, the 16-frame cycle | 0.93-0.95% | 1.96-1.98% |
    | 16 at 240 fps, 32 | 0.91-0.93% | 2.27-2.31% |
    | 16 at 240 fps, 64 | 0.93-0.99% | 2.04% |

    So noise goes with rays a second as expected, and stretching the cycle to keep the long spiral at 1,024 points (the first plan) buys nothing: the cycle stays 16 and no shader changed.
  - **A bug on the way:** the ladder's first form stepped up only at the whole step and failed its own test, since a steady 1/240 s comes back as 239.99998 fps. A real 240 jitters both sides, so a step up is taken 5% short of it.
  - Off by default (a budget of 60 at 240 fps crawls like 60 fps, three times today's 240), so the golden images are unchanged.
- `lightlab-default` (Oct 20 2026): the light lab is the default game (first in the registry), so a plain start, `make run` and double-clicking the packaged exe open it. The zip's `Demo.bat` starts the demo (it had `LightLab.bat` the other way round). The user's choice.

#### Adaptive memory, sharing and the quality presets

- `ddgi-adaptive-memory` (Oct 20 2026): with the ray budget at 60 the user saw a fair bit more noise, which is what 60 fps looks like without one: the 2 s / 64 rays were judged at 240 fps.
  - **Reading the probes differently was weighed first and set aside.** Every probe traces the same directions each frame and neighbouring texels share their rays, so their noise is shared and blurring them averages little of it away. TAA's memory (under 0.1 s) is far shorter than the crawl.
  - **The noise is in each texel's memory, so that is where the change went.** A scratch experiment on `ddgi_test`'s dark-room texel (200-320 s runs, steps between states 40 s apart) came first.
  - **A first version** kept an 8 s memory and pulled the stored light towards a 0.2 s window's mean while they differed by more than 4 of the window's standard deviations. It was as calm as a fixed 8 s, never firing falsely, but a doubling took 10-17 s, since it stopped pulling while most of the gap was left.
  - **Restarting the history instead** (a running mean from the window's mean, as denoisers' history lengths do) fixed that for clear changes: a lamp switched on or off at 60 fps came in within about 1 s instead of 4.6, crawling a quarter as much with 8 s.
  - **But near the information limit nothing helps:** an 8 degree patch at 60 fps is seen by about 0.3 rays a frame, so its doubling cannot be told from luck in a fraction of a second, and the filter fell back to its long memory (17-18 s). Slow drifts lag by it too (ramp error 15-18% against 5-6%).
  - **Hence:**
    - a 4 s long memory in the measurements (crawl halved, switches within 0.5-1.1 s, the worst case 9 s)
    - the noise kept with the long memory's share, starting from the stored light (the fixed blend now writes alpha 0, so switching on mid-run does not jump)
    - off by default for the user to judge on screen, so the golden images are unchanged; switched on, the light lab's probe views moved by at most 5 of 255 and the validation layers reported nothing.
  - `probeLuminance` replaced the luminance `hitLightStrength` computed itself, on both sides.
  - Sharing light between neighbouring probes (independent rays per probe, then a visibility-aware blend) is on the roadmap as the next step, since only more information beats the dark room's limit.
- `ddgi-probe-sharing` (Oct 20 2026): at 30 fps with 64 rays and 2 s the user still saw a lot of noise in the dark back room, and found a longer memory's lag unacceptable even with the adaptive memory, so the cure had to be more information, not more memory.
  - **More rays measured poorly at 30 fps** (`ddgi_test`'s dark-room model, 2 s): 64 rays 1.41%, 128 0.98%, 256 0.85%, against 0.58% for 64 at 120 fps with the same rays a second. Each update is a big step at 30 fps however good its estimate.
  - **A suspected leak, before building:** the user's screenshot of a warm glow along the back room's ceiling against the partition raised a leak; dense probes made it go away.
    - A temporary capture of that view with each suspect changed apart measured the strip along the partition: the visibility floor at 0, the self-shadow bias at 0 or 0.6, and no probes above the roof changed nothing (30.9 red against 20.4 further away, unchanged within 1).
    - With the doorway closed the strip became the ceiling's darkest part (4.5 against 6.9), as an inside corner should.
    - So there is no leak through the wall: the probes beside the doorway, 35 cm from the partition and 50 cm below the ceiling, see the sunlit floor through the doorway at an angle the corner above cannot, and the corner reads mostly them (a parallax error, on the roadmap as finer probes at openings).
  - **Sharing was then built as planned:**
    - First each probe's own turn of its rays. With one turn for all, neighbours err alike and averaging removes nothing: seven probes in the model at 30 fps, weight 0.5, 1.41% with one turn, 0.54% with their own, theory 0.40 of it.
    - Then a pass averaging each probe with its six face neighbours it can see, into separate atlases so the blur never feeds back into the probes' history.
    - The turns went in a per-probe buffer of identity rows when off, rather than a flag in the full push constants, so sharing off is bit for bit the old picture.
  - **Measured** in the light lab's back room with temporary capture overrides (frame time 1/30 s, warm-up 600-1203 frames, half-float captures 3 frames apart):
    - crawl 1.61% RMS without sharing, 0.77% at 0.5, 0.73% at 1, against 0.88% at 120 fps without, so 30 fps with sharing is calmer than 120 fps was
    - the room's mean light rose 7% at 0.5 (light borrowed from brighter neighbours it can see)
    - the back room's floor seen through the doorway brightened most (up to 31 of 255 in the sunlit-room view).
  - Off by default for the user to judge; validation layers clean with it on.
- `ddgi-fast-light-toggle` (Oct 20 2026): a setting to stop keeping fast lights apart (`fastLights`, on by default), asked for by the user now that the adaptive memory exists.
  - Off, `probeHitLights` lists every light as still, so the flashlight's bounce goes into the still atlas under its memory, which with the adaptive memory restarts where the change stands out from the noise.
  - Golden images unchanged.
- `gi-defaults` (Oct 20 2026): the user's refined settings became the defaults, taken from their `settings.cfg` without further measuring:

  | Setting | New default | Was |
  |---|---|---|
  | Rays a probe | 128 | 64 |
  | Ray budget | 60 updates a second | off |
  | Adaptive memory | on, with a change threshold of 8 standard deviations | off, 5 |
  | Probe sharing | 0.5 | off |
  | Fast irradiance time constant | 0.1 s | 0.05 |
  | The light lab's sun clock | still by default (0 h/s) | 0.025 |

  - The clock-pinning test now sets it moving itself.
  - The tests of the schedule's own rules (CPU, `plainProbeSettings`) and the GPU's real-pass checks now set the plain probes (64 rays every frame, the fixed memory, no sharing) explicitly.
  - The light lab's views with probes were saved anew.
- `gi-quality-presets` (Oct 20 2026): the quality presets set the light probes' rays too, the same for still and fast probes, as the user asked: Low 32, Medium 64, High 128, Ultra 256, the rest of global illumination as tuned.
  - That clashed with Ultra being the defaults (128 still, 256 fast rays then).
  - The user chose to make the defaults High, keeping the 4096 cascades (so High, the default, no longer saves shadow memory for typical GPUs; Medium's 2048 cascades do), with the fast rays down to 128 (only the flashlight golden view changed, by at most 1 of 255).
  - Ultra is now High with 256 rays.
- `low-preset-cascades` (Oct 20 2026): on Low the user saw light seeming to clip through the light lab's walls, a ragged bright fringe where the sunlit room's partition meets the ceiling and stair steps on the doorway jamb.
  - It was first taken for the probes (32 rays), but a longer distance memory changed nothing, and Medium with Low's probe rays did not show it.
  - It was the sun's shadows: 2 cascades of 1024 over 60 m, leaving 30 cm walls a texel or two thick.
  - 1024 cascades saved no memory (the atlas is 128 MB for any cascade up to 2048), so Low's cascades went to 2048. Low stays cheaper than Medium in cascade count and reach, lamp shadows, bloom and probe rays.
  - Low is not the default, so the golden images are unchanged.
- `low-preset-cascade-count` (Oct 20 2026): with 2048 cascades Low still clipped a little; the user found 3 cascades (Medium's count) fixed it, so Low has three, reaching 60 m against Medium's 120.
- `gi-preset-sharing` (Oct 20 2026): the quality presets set sharing between probes too, as the user asked: Low 0.5, Medium 0.3, High 0.1 (so the default drops from 0.5 to 0.1), Ultra 0, more sharing making up for fewer rays. The light lab's views with probes were saved anew.
- `ddgi-fast-sharing` (Oct 20 2026): the only flicker left for the user was a flashlight at 30 fps (the fast atlas's 0.1 s memory averages three frames there).
  - So the fast atlas got its own sharing weight, `fastProbeSharing`, 0.5 by default and untouched by the presets; the pass already averaged the two atlases apart, so it was a second weight in its push constants.
  - Only the flashlight golden view changed (at most 5 of 255). Not measured; judged by the user.

#### Scene events

- `ddgi-scene-events` (Oct 20 2026): the roadmap's scene events, with a ceiling lamp in the light lab's back room (L) to try them on, as the user asked. Decisions:
  - **Restart, not the paper's multipliers.**
    - 2021 scales every probe's hysteresis by 0.85 for 4 frames or 0.5 for 10.
    - The adaptive memory already has a restart (a running mean, n / (n + 1)), and a game's event says outright what the adaptive memory has to prove from its noise, so an event restarts the probes it covers and they average the frames since.
    - The fixed memory and the distances have no history of their own, so the CPU counts frames since each probe's event (`GpuProbeRestart`, a per-probe buffer in each frame in flight's upload memory, since `GpuProbeVolume`'s 128 bytes of push constants were full) and they keep min(keep, n / (n + 1)).
  - **Regions**, as the user chose from the two options (and as the paper suggests): a restarted probe is noisy for a few frames (one frame's rays at first), so an event carries a box (the lamp's reach) and only probes inside restart; a time-of-day jump uses everywhere.
  - **The game signals**, as the user chose over the engine spotting light switches itself (which could not see geometry changes): a `ProbeEvent` component lasting one frame, gathered with the lights and cleared by the engine after the frame is built.
  - **The first version failed:** it restarted only the stored light and kept the change detector running with the old noise.
    - In the dark room model a lamp then came in in 0.27 s at 60 fps, not the fixed memory's 0.044: the dark room's tiny noise made the lit room's own noise pass for change after change, each restarting from a window that still held the darkness.
    - Taking the noise as at least the light did not help (the first frame's estimate is often the darkness itself, with no ray on the lamp).
    - What worked: for the change window's length after the event, the window's mean is the plain mean too, the noise is measured afresh and no change is looked for (`restartedProbeBlend`, chosen by `adaptiveProbeUpdate`, which the shader calls).
    - Measured: 0.044 s at 60 fps, 0.008 at 240 (0.81 and 0.96 s with the adaptive memory alone), the settled crawl unchanged.
  - **Golden captures** repeat one frame description 960 times; its events now go to the first warm-up frame only, or every repeat would restart the probes.
  - **The lamp:** warm, intensity 2, range 7 m, picked by eye in a new golden view (`back-room-lamp`); its panel casts no shadow so it blocks neither its light nor probe rays.
    - Off at the start, but the panel is still drawn, so the `flashlight` view, which shows it at the top, changed.
    - Two views with the building in sight changed by a hair (overview: 36 pixels past tolerance, at most 4 half-float steps; sun-disc within tolerance): the acceleration structure holds one more instance, so a few probe rays came out a hair different, though they skip the panel.
    - With the panel hidden, every reference matched bit for bit, so the events themselves change nothing until signalled. The light lab's references were saved anew.
  - **Moving the clock by hand** (the slider) past 0.1 h signals a light change everywhere; the clock running by itself does not, its sun moving smoothly.
  - **More sharing during a restart:** the user asked for sharing to rise to 0.5 while a restart runs, against the noise of averaging few frames, and chose a fade over a step back (a step would sharpen the bounce in one frame).
    - A restarted probe's still weight fades linearly from `eventProbeSharing` (0.5) back to the preset's over the restart's period, about 2 s, only for the probes in the region.
    - Building it showed that an event on a frame with fresh atlases (the `back-room-lamp` view switches its lamp on before the first frame) started a restart that ran on, sharing more, through the warm-up. Such frames keep nothing, so they now start no restart, and that view's reference was saved again (unchanged to the eye).
  - **The soft restart:** at 30 fps on Low the user saw a lot of noise in the sunlit room when switching the lamp. The event's region, a cube of the lamp's 7 m reach, covered the whole sunlit room, whose probes restarted though the lamp hardly changes their light (Low already shares 0.5, so the boost added nothing there).
    - Offered: restarting only the back room, more event sharing, or a softer restart. The user chose the soft restart, with a slider (`eventMemorySeconds`): through a restart a probe keeps at least the share a short memory keeps each frame.
    - Measured in the dark room model at 30 fps:
      - a restarted probe whose light did not change errs 22.5% over the first half second (1.4% left alone), 16.9% at 0.05 s, 13.4% at 0.1 s, 8.1% at 0.3 s
      - while a switch takes 0.05, 0.14, 0.26 and 1.45 s to reach 90% (a running mean keeps its first samples, so the old light fades with a tail).
    - 0.05 s is the default. Even softly a restarted probe is far noisier than one left alone, so the light lab's region is the larger lever.
    - Golden images unchanged. The user preferred to keep the general mechanism rather than tune the light lab's region.
  - **A growing memory:** the lamp then went dark far slower than the flashlight.
    - A restart averaged the frames since the event as a plain mean, so its first frames, holding lamp light (the soft restart's share, and the bounce of last frame's probes, which probe rays read), faded only as 1 / time: at 30 fps 1.7% of the lamp was left after a second, as bright as the dark room itself.
    - Now a restart's memory grows with the time since the event, its time constant `eventMemoryGrowth` (0.5) times that time, so an early frame's weight falls as the square of the time ratio instead.
    - Measured in the dark room model at 30 fps: within 1% of darkness after 0.30 s instead of 1.32 (the flashlight 0.46 s), for a restarted probe whose light stays 20.6% instead of 16.8% over the first half second and 4.5% instead of 3.3% to 4 s; 0.25 was 0.20 s for 26.3% and 7.2%.
    - A slider (`eventMemoryGrowth`) lets the user judge. Golden images unchanged.
  - **The change check:** dim areas the lamp barely touches still flickered at each switch, since every texel in the region restarted, however little its light changed. The user's idea: change quickly where the change is big, slowly where it is not.
    - Built as a check (adaptive memory only): for 0.5 s after an event each texel compares the mean of the frames since with its stored light in units of its own noise (shrinking as 1 / sqrt(frames)) and restarts only past 4 of them.
    - A restarted texel is marked by a negative history while the restart runs, so the growing memory follows its own frames since its restart.
    - In the dark room model at 30 fps, a texel the lamp makes 15% brighter or leaves alone now flickers 0.74%, as with no events, instead of 13.9% and 14.9%. The lamp itself comes on in 0.16 s and goes dark in 0.28 s (3 gave 0.09 and 0.13 s; 5, 1.3 and 4.9 s, a sparsely lit texel's noise taking too many frames to tell apart).
    - Measuring it showed the soft restart now made restarted texels noisier (24.2% against 22.8%: keeping a restart's single first frame longer keeps its noise), so it applies only where texels still restart outright: the fixed memory and the distances.
    - Golden images unchanged.
  - **The flashlight's switch as an event,** as the user asked.
    - Its bounce lives in the fast atlas, which the check (on the still atlas) never touched, so the fast atlas now restarts with the still one, outright (no noise measured there, and its 0.1 s memory makes a restart cheap).
    - `Flashlight::toggle` signals a light change over the beam's 50 m reach, which the check makes harmless for still light it hardly changes.
    - Dark room model: on within 0.02 s instead of 0.17 at 30 fps, dark at once instead of 0.43 s, no more flicker.
    - Golden images unchanged (the flashlight view's switch falls on its fresh first frame).
- `ddgi-event-spam` (Oct 20 2026): spamming L, at low frame rates especially, left the lamp to dim very slowly.
  - **Reproduced** in the dark room model (the switching harness gained short states and the renderer's own rule for ending restarts, `advanceRestartFrames`, made public for it).
    - A sparsely lit texel needs about 13 frames to tell its light going out from its noise, so each switch's check was cut short by the next and never restarted it, and the light took the long memory to go dark once the switching stopped (13-16 s).
    - A single switch at 15 fps failed too, the 0.5 s check being 7 frames (18 s).
  - **Two fixes:**
    - A texel the last event restarted (still marked) restarts at once at the next, unchecked, since the same light is most likely being switched back.
    - The check lasts at least 16 frames (`minEventCheckFrames`, `eventCheckFrames`).
  - Now dark at once after switching every 0.1-1 s at 15 and 30 fps, 0.73 s after one switch at 15 fps. Golden images unchanged.

#### Relocation and the sky

- `ddgi-probe-relocation` (Oct 20 2026): probe relocation and probe states, the 2021 paper's rules with the reference code's numbers (256 fixed relocation rays, at most 5 steps, inside past 25% back faces, an offset limit of 0.4 of a cell, too close under 0.1 of the smallest spacing, nudged by up to 0.2 of it). Decisions:
  - **Relocation runs on the GPU** with ray queries against the static level, not on the CPU against the game's `StaticLevel`.
    - It sees exactly what the probes see, the renderer never sees the collision level (and a game's collision meshes may differ from what is drawn), and the dense volume (19,035 probes x 256 rays x up to 6 traces) would take seconds on the CPU in a debug build.
    - The records come back through read-back memory, read by the next frame after waiting for the GPU (a one-off hitch, only after start-up, a new volume, a geometry change or switching relocation).
    - The CPU then owns each frame's states: it knows them for the readout and for restarting woken probes, and the rules are tested directly.
  - **What is static is marked by the game** (`ModelRenderer::isStatic`, carried by `DrawItem` and the new ray mask bit `RAY_MASK_STATIC`), defaulting to moving.
    - A forgotten mark costs only probes kept awake, while guessing from components (a `PreviousTransform`, spinners, fireflies) would miss the next kind of mover and leave probes asleep beside it.
    - The light lab marks its ground and blocks, the demo its ground.
  - **A probe that sees no static surface at all sleeps** instead of being switched off, against the paper: something that moves may still come near it, and a level with nothing marked static then lights its movers as before (every unmarked draw wakes the probes round it).
  - **Vigilant** means a front-face hit inside the probe's own cells (one spacing each way round its grid point) grown by the self-shadow bias's longest reach.
    - The cell a surface's biased point falls in picks the probes, so these are exactly the probes some surface reads.
    - The pass records how far the nearest front face lies beyond the cells, and the CPU compares that with the bias every frame, so changing the bias needs no new pass.
  - **Awake:** a sleeping probe within a mover's bounds grown by the same and `wakeCells` (1) more cells, so it has settled by the time the mover arrives.
  - **A probe that starts being traced, or moves, restarts outright** (no share of its old light kept, every texel restarted unchecked) through the scene events' restarts.
  - **Readers:** sampling skips off and sleeping probes, and a point with none round it gets the flat ambient; sharing skips them and measures between moved probes.
  - **The debug balls** sit where the probes moved, off ones red, sleeping ones dark blue, awake ones with a yellow rim. The F4 readout gained a probe line (probes, traced, awake, asleep, off, moved).
  - **Measured** in the light lab (golden warm-up frames, release build, RTX 5080):
    - The 1 m volume moves and switches off no probe (none lies in a wall or within 10 cm of one), and 392 of 960 sleep, the probe pass 0.036 ms instead of 0.050.
    - The dense 20 cm volume switches off 350 probes buried in walls (only 5 could move out: the limit is 8 cm there, less than half a 30 cm wall) and puts about half to sleep, 0.25 ms instead of 1.0.
    - With relocation and states both off the light lab's views match the old references within tolerance (a few hundred pixels one half-float step apart: rounding in the reshaped shaders, the same pixels as before the level was marked static).
    - With them on its views changed by at most 3 of 255 on screen, and the light lab's references were saved anew.
- `sky-atmosphere` (Oct 20 2026): the sky and the air (Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique").
  - **What was built:** an `Atmosphere` component (Earth's air by default, in km as the paper) and three tables:
    - a transmittance table (256 x 64, made once)
    - a multiple scattering table (32 x 32, the paper's geometric series 1 / (1 - f), made once)
    - a sky view table (192 x 108, every frame).
  - **What it does:** drawn over the background, lighting probe rays that escape, dimming and reddening every directional light on its way down, and adding the light it gives an open surface to the flat ambient (computed on the GPU, the frame's own ambient plus the sky's cosine-weighted mean over the sky above).
  - The light lab switches to it by default (its hand-made day stays as the fallback, `atmosphere` off).
  - **Decisions, with the user** (recommended and accepted):
    - the sky pass draws the sun's disc (its radiance through the air, so it reddens at sunset; the light lab's disc entity only without the atmosphere)
    - the flat ambient from the sky on the GPU
    - no aerial perspective yet (in the light lab's 400 m world the air in front of the farthest tower lets about 99% through, so the haze would not show; it waits for kilometre scales, phase 13)
    - the light lab only (the demo keeps its flat night sky and its golden images).
  - **Measured and changed from the paper:**
    - Sampling each step at its midpoint, not 0.3 of the way in (straight up, 0.3 made blue's optical depth 2.5% too deep, the light kept 0.9% low against the closed form; the midpoint is within 0.05%), with steps growing as the square of their number (even 2.5 km steps put the whole 1.2 km aerosol layer in the first).
    - The distance to a sphere from the radius and the cosine rather than a position, its near root found from the far one (at 6,360 km a float's last digit is half a metre, and the squares' rounding several square kilometres, so the GPU and the CPU disagreed by a metre on rays grazing the ground).
    - 1 - exp(-depth) by its series below 1e-3 (in the thin air near the top the GPU's exp rounded to exactly 1 and the multiple scattering lost those steps).
    - The sunlit ground added where a sky view ray ends on the planet (without it the planet beyond the light lab's 400 m field showed as a dark band below the horizon). The light lab gives the planet its grass's albedo, so the land beyond the field matches it.
  - **Units with no fudge factors:** a sky pixel is pi times the sun's value times the scattering integral per unit illuminance, so the sky's brightness follows from the sun's. The light lab's sun is now its light above the air, 4 by default, so in the default mid-afternoon (the sun 25 degrees up, 71% of green getting through) the ground gets about the 3 it had.
  - **Cost:** the sky's tables and ambient cost 0.015 ms a frame on the RTX 5080 (the sky view table is the same size at any resolution).
  - **Golden images:** every light lab view changed (sky, sun colour, ambient) and its references were saved anew; the demo's matched bit for bit, its flat ambient now read from a buffer instead of the frame's uniform block.

#### Cascades for large worlds

- `ddgi-cascades` (Oct 20 2026): cascades for large worlds.
  - **What changed:** the probes no longer live in a box a game places but in nested grids round the camera, the densest 0.5 m apart and each further one twice as far apart (four by default, 21 x 13 x 21 probes each: the densest lights 3.6 m round the camera at full weight, the fourth 29 m), all moving with it. So light bounces wherever the camera goes and no level places or tunes probes.
  - **Decisions, with the user:**
    - Cascades only, no hand-placed volumes beside them: the user wants lighting emergent, with as little per-level tuning as possible. A fixed volume per room stays possible later, since the code blends any list of volumes.
    - The light lab's two hand-placed volumes (the 1 m one round the building and the 20 cm test) are gone.
    - Scrolled-in probes start from the coarser cascades' light (seeds), with a switch to compare.
  - **Scrolling:** each cascade is a window on a lattice fixed in the world (multiples of its spacing from the origin), its middle the lattice point nearest the camera.
    - When the camera crosses into another cell, the plane falling out behind is reused for the plane coming in (a circular buffer: `ProbeVolume::scroll`, slot = (grid place + scroll) mod count).
    - So every other probe keeps its slot, its light and its history, and only the new plane starts.
    - Corners, neighbours and event regions are found by grid place, never by slot (the slot next to the last plane wraps to the far side).
  - **The blend is centred on the camera**, not on the window, so it moves smoothly and nothing pops when a plane jumps: the densest first, each cascade weighted by its fade times what the denser ones left, the flat ambient last.
    - Its reach was derived rather than tuned. The camera lies up to half a cell from the window's middle, so the nearest outer plane is at least (count - 1) / 2 less half a cell away.
    - The weight ends one more cell and the self-shadow bias's reach short of that, so the cell round any point it lights (found from its biased point) never touches an outer plane, where a scroll brings in probes that are new (tested over 20,000 random cameras and points).
    - [2021]'s rule (shrink the blend by a cell) left the cell round a point touching the new plane by up to the bias's reach.
  - **New planes wait for relocation:** a plane the scroll brings in waits off for its own relocation, then starts.
    - Relocation used to be read back the next frame after waiting for the GPU (a hitch, fine once at start-up); with a cell crossed every few frames when walking that would stutter.
    - So each frame in flight now has its own read-back records, read when that frame comes round again (its fence has passed: no waiting), and each record is kept only if its probe still stands at the lattice point it was asked for at.
  - **Seeds:** a probe that starts (scrolled in, woken, newly vigilant) is seeded by a small pass (`ddgi_seed.comp`) with the coarser cascades' light where it stands, in each texel's direction (the irradiance a surface there facing that way would get), counted as 4 frames of its own history and fading as its own rays come in.
    - Measured in the dark room model (128 rays, 60 fps), the error over its first quarter second: 13.3% from its rays alone, 9.6% seeded 20% off, but 24% seeded with twice or none of the light (a coarse probe that sees through a doorway the fine one cannot); from half a second on all within half a percent.
    - Kept on, with `seedProbes` to compare by eye.
  - **One set of atlases holds every cascade**, one after another (the grid counts.z times cascades layers deep), so every pass runs one dispatch over all of them. A per-frame buffer of cascade records (origin, spacing, scroll, first probe, weight) replaces the volume's numbers in the push constants and the frame block, which lost its probe origin and spacing.
  - **Quality presets:** High (the default) and Ultra four cascades of 21 x 13 x 21 from 0.5 m, Medium four of 17 x 11 x 17, Low three of 15 x 9 x 15 from 1 m.
  - **Measured** in the light lab (golden views, debug build, RTX 5080), against the single 1 m volume:
    - the probe pass 0.22-0.50 ms instead of 0.034-0.051 (3,700-5,600 of 22,932 probes traced, the rest asleep or off)
    - the whole frame 0.47-1.11 ms instead of 0.30-0.59
    - the engine's video memory 734 MB instead of 486 (about 250 MB of probes).

    About a third of the probes are off: a window centred on a camera at eye height reaches as far below the ground as above it. From the high overview camera no cascade reaches the ground and every probe sleeps (the flat ambient lights the view, as it lit everything outside the old volume).
  - **Found by the user walking the light lab at sunset:** dark blotches and sharp bands on the ground.
    - Every cascade's lattice started at the world's origin, so each had a layer exactly on the ground (y = 0).
    - A probe exactly on a surface starts its rays on it, where they hit or slip through by rounding, and rounding in the probes' positions (measured from the camera and back) put some a hair above and some a hair below, which saw only the ground's underside and lent darkness. The old volume had sat on half-metre marks for this reason.
    - Now every lattice is offset half the densest spacing (0.25 m) from the world's origin, the same in metres for every cascade so they stay nested, which keeps probes off whole and half metres, where levels put their floors and walls; a capture of the same view showed the ground smooth.
    - The probes' debug balls also scaled with each cascade's spacing and buried the view, so they show one cascade at a time (`shownProbeCascade`).
  - **Golden images:** every light lab view changed (finer bounce light in the rooms, the grass's green bounce on the towers' shaded sides, which the old volume did not reach) and its references were saved anew; the demo's matched bit for bit.
- `ddgi-relocation-surfaces` (Oct 20 2026): the user found the house lighting worse since cascades, looking from the inner doorway at the window: a dark block on the wall below and left of the window and a bright smear down its near edge.
  - **Narrowing it down:** a capture of that view on the build before cascades and on the current one showed it. Switching suspects apart in scratch builds:
    - relocation off removed the smear but left the wall dark and flat
    - a self-shadow bias of 0.6 changed nothing
    - moving the lattice off quarter metres moved the patches
    - 1 m spacing (the old layout) gave the old picture.
  - **The cause**, found by the CPU mirror of relocation run against the lab's own triangles: the densest lattice (x.25 and x.75 m) puts a layer of probes inside the 0.3 m window wall and two columns exactly on the planes of the window's sides, which inside the wall are the seams where its boxes touch (two faces on top of each other).
    - Relocation rays started at distance 0, so from a seam a ray met the neighbouring box's face from outside at once.
    - The probe took itself for one in the open beside a surface, neither moved nor turned off, and lent the wall the boxes' dark insides (or, at the window's edge, the sunlit ground).
  - **The fix:** relocation rays now skip hits nearer than 1 mm (`DDGI_RELOCATION_MIN_DISTANCE`), so such a probe sees the solid's real inside and steps out into the room; probes standing in the open window hole stay.
    - Chosen over another lattice offset, since levels put walls anywhere and any offset lands some probes on some level's faces.
    - A light lab test relocates every densest probe in and round the window wall against the level's triangles (`traceRelocationRay`) and failed before the fix.
  - **Golden images:** `sunlit-room` (up to 38 of 255, on the window wall), `flashlight` and `back-room-lamp` (at most 1 of 255 in the final image) saved anew; every outdoor view and the demo's matched bit for bit.
- `ddgi-densest-cascade` (Oct 20 2026): the user found the bounce light's detail fell off too soon round the camera.
  - The densest cascade lit only 3.6 m round the camera at full weight and 1.6 m up and down, so from eye height the floor at one's feet, the ceiling and a room's far walls already read the 1 m probes.
  - Of three options (taller windows for all, every cascade larger, or only the densest larger) the user chose the densest alone, since detail matters most near the camera: 31 x 17 x 31 probes (6.1 m and 2.6 m), the coarser three still 21 x 13 x 21; Medium a densest of 25 x 15 x 25, Low 19 x 11 x 19.

  Decisions:
  - **The atlas layout:** the atlases' rows were one layer of a cascade, which needs every cascade the same width, and a 31-wide layer of distance squares would be 17,298 texels, past the largest image side.
    - Now every probe is numbered straight through the cascades and the squares are wrapped into a near-square sheet (`probeAtlasGrid(probes)`: 65,535 probes fit in 4,608 texels a side).
    - Each cascade's record carries its own counts (`GpuProbeCascade` 96 to 112 bytes); the push constants carry the sheet's columns and rows.
    - A probe's cascade is found by walking the at most six records (`probeCascadeHolding`), rather than a lookup table to keep in step.
  - **Checked that the rework alone changes nothing:** with the densest set back to 21 x 13 x 21 and the old sheet's shape forced (273 x 84 squares) every golden view matched the old references bit for bit. With the new near-square sheet they differed by a few half-float steps (up to 25 in one view's scene image), the GPU's bilinear filter rounding texture coordinates on a sheet of another size, compounded by 960 frames of bounces.
  - **The GPU cascade tests** now use cascades of different sizes. Their first version showed that a densest window reaching past the next cascade's full weight seeds the planes it brings in partly from the flat ambient when no third cascade covers them; the real layouts always have one, so the test's coarser cascade was widened instead.
  - **Measured** (release, RTX 5080, light lab golden views, against the same build at 21 x 13 x 21): the probe pass 0.22-0.56 ms instead of 0.18-0.49 (+13-25%; 3,346-5,793 probes traced instead of 2,826-5,028), whole frames up to 0.07 ms longer, the engine's video memory 795 MB instead of 700.
  - **Golden images:** every light lab view with probes saved anew (finer bounce light further from the camera); the overview, which no cascade reaches, and the demo's matched.
- `ddgi-relocation-nudge` (Oct 20 2026): the user saw the light lab's back room glow like the outdoors through the front door from 12 m or more away, and dark again up close.
  - **Tracked down step by step:** captures from the door at 6, 10, 15 and 25 m on this build and on the one before showed it was there since cascades. In scratch builds:
    - painting the flat ambient's share magenta showed the light came from probes
    - painting each cascade its own colour showed the room read the 4 m cascade from 15 m
    - capping the distances' variance (a guess at Chebyshev leaking past the eaves) changed nothing
    - relocation off made the room dark.
  - **Why**, from the CPU mirror of relocation against the level: the 4 m cascade has one probe inside the back room, its grid point 0.23 m over the floor, under its too-close threshold (0.1 of 4 m).
    - The 2021 reference's rule moved it towards the farthest front face it saw, which lay out through the front door.
    - Five steps walked it 1.6 m (its limit) to 0.85 m from the doorway, where it saw mostly the sunlit outdoors and lent that to the room's back wall, which faces the door.
  - **The fix:** a probe too close to a surface now steps straight away from it until it stands 0.2 of a spacing off it, which is what the rule is for and cannot herd probes into openings.
    - The same-surface guard (`DDGI_SAME_SURFACE_COSINE`) is gone with it.
    - The nearest surface's direction is that of the relocation ray that met it nearest, a few degrees off its normal, so a probe drifts a few centimetres sideways as it rises.
  - **Tests:** a probe low over a floor beside an open doorway rises rather than walking out, and the back room's 4 m probe stays more than 2 m from the door; both failed before.
  - **Not fixed by it:** from about 25 m the back room lies beyond the coarsest cascade's full weight (29 m) and takes the flat sky ambient.
  - **Golden images:** `sun-disc` (up to 35 of 255: the room behind its door is dark now, where the old reference showed the leak), `towers` and `sunset-hdr` (the coarse cascades' light on the house and ground) saved anew, and the indoor views too (at most 1 of 255); the demo's matched.
- `ddgi-far-cascades` (Oct 20 2026): the user asked whether lighting is dynamic far away. Direct light and sun shadows are (to 200 m), but bounce light only reached the fourth cascade's 29 m; beyond, the flat sky ambient lights everything as open sky.
  - **The change:** the cascades already allowed six, so the defaults now use them: two more 21 x 13 x 21 cascades 8 m and 16 m apart, carrying bounce light to 58 m and 116 m at full weight (Medium five, to 8 m; Low four, to 8 m).
  - **What it does:** they are too coarse to see into a house (an 8 m lattice may have no probe inside one), but light landscapes: in the overview, which no cascade reached before, the towers' shaded sides now take the grass's green.
  - **Measured** (release, RTX 5080, light lab golden views, against four cascades): the probe pass 0.36-0.78 ms instead of 0.22-0.56 (5,261 probes traced instead of 3,346 in the towers view; the far cascades' probes near the ground), whole frames 0.14-0.23 ms longer, the engine's video memory 828 MB instead of 795.
  - **Golden images:** every light lab view saved anew; the demo's matched.

#### Night, the eye and the street

- `night-sky` (Oct 20 2026): the user asked for a moon and a starry sky at night, and chose a real star catalogue and artistic brightness dials (no exposure yet).
  - **The moon's place** comes from Meeus's mean elements and largest terms; checked against 2026's two eclipses (the model aligns both within 0.05 degrees) and the equinox and solstice (0.005 degrees).
  - **Its phase is geometry:** the disc in the sky pass is shaded by Lommel-Seeliger's law (the closed-form disc average checked against a numerical one to four digits), and its light follows Allen's measured phase curve (a quarter moon 9% of full).
  - **Its sky reuses the sun's:** scattering is linear in the light, so the sky view table holds a second sky lit by the moon, stacked under the sun's (an atlas, like the probes', rather than a layered image, so no binding or image class changed); the other two tables are shared.
  - **Directional lights are now dimmed by the air before they are ranked**, so once the sun sets the moon takes the shadow cascades.
  - **The stars** are the Yale Bright Star Catalogue (9,096 stars, trimmed to 285 KB).
    - Coloured by Ballesteros's temperature and Planck's law against the CIE curves.
    - Turned by one rotation from the sidereal angle (the sun's hour angle plus its right ascension).
    - Drawn as tent-weighted splats so a star's light stays whole as it crosses pixel borders; the air dims them, the moon hides them.
  - **Found on the first golden run:** the boosted stars (thousands of times real) showed through the day sky, so the light lab fades them in from the sun 4 to 14 degrees below the horizon.
  - **Moonlight:** the first night view was nearly black at the planned moonlight of 0.05; the default is 0.25 (the full moon above the air; the sun is 4).
  - **The clock** now carries past midnight into the day and the year (`advanceLabClock`), so the moon moves on through a running night.
  - **Not measured on their own:** the moon's sky doubles the small sky view table (192 x 108 texels, 30 steps each), and the stars are 9,096 instances of six vertices.
  - **Golden images:** a new `moonlit` view (day 88, 21:00, facing the waxing gibbous moon); every other light lab view saved anew (the day's young crescent moon adds a faint sky and light, at most 2 of 255 in the final images); the demo's, which has no atmosphere, matched.
- `eye-adaptation` (Oct 20 2026): trying the night sky, the user found the pillars and ground looked lit only by the sky, not by the moon.
  - **Checked:** the game handed the renderer the right moon (56 degrees up, south-south-east, 0.09 after the air), and with the moon as bright as the sun the night drew exactly as a day under a high southern sun, so its light, direction and shadows worked.
  - **The cause:** at 1/35 of the sun's light every surface sat in the tone curve's toe, where lit and shaded sides squeeze together and lose their colour.
  - **Choice:** of three fixes (eye adaptation, a night exposure dial in the lab, a brighter moon), the user chose eye adaptation.
  - **How it works,** all on the GPU, read by tone mapping the same frame:
    - a histogram of the scene's luminance in stops (256 bins, shared-memory atomics per workgroup)
    - the mean of its middle (50th to 90th percentile: the black sky and the moon and bulbs left out)
    - a target that makes up a share of the difference from a reference
    - an exponential ease, slower into the dark (3 s) than into the light (0.7 s).
  - **Tuned** by the golden views' measured means (daylight -1.2 to -1.9 EV, the sunlit room -6.1, the moonlit night -6.6, the demo's night -5.3 to -7.4, its moon view -16):
    - the reference 0.3 keeps the daylight towers as they were
    - strength 0.75 made the moonlit night a dull day, 0.5 keeps it a night
    - the highest exposure +4 EV (not 6) keeps a black-sky frame from being boosted 64 times.
  - **Found on the way:** the main pass left the scene image readable by fragment shaders only, and a barrier from one read state to another carries no memory visibility, so it now leaves it readable by compute shaders too.
  - **GPU tests** match the histogram pixel for pixel and the exposure to 1e-6.
  - **Golden images:** every view saved anew, both games (the demo's night brighter, the sunlit room's interior brighter, the daylight views within a few steps).
- `light-lab-street` (Oct 20 2026): a test ground for many lights at night in the lab's real setting (sky, moon, cascades, eye adaptation), where the demo's lamp field has none of them.
  - **The user's choices:**
    - Point lights with a solid shade over each bulb (so every lamp has soft shadows and its own cube map shapes the pool, with no cone to tune), over spot lights (one tile each, but only 8 may cast shadows and a 140 degree beam stretches one tile badly) or no shadows.
    - A 300 m street with lamps every 10 m (all within the probes' 116 m when standing in the middle), over 750 m at a real 25 m.
    - The automatic switch at dusk plus K to flip it until the sun next asks for a change.
  - **The windows** are glowing static panes with no light component: the probes' rays bring their glow back, so they light the street through global illumination alone.
    - First night view: the windows at a radiance of 1 were 30 times the lamp-lit road and clipped white under the exposure the road sets; dimmed to about 15 times, they keep some colour.
  - **The moonlit view**, facing the moon in the south, now looked at the north row's backs, so its camera moved south of the street (z = 80).
  - **Measured** (debug exe, RTX 5080, 2560x1440, the street-night view, 100 captures):

    | Street | Main pass | Probe pass |
    |---|---|---|
    | dark | 0.91 ms | 0.62 ms |
    | lamps lit, casting no shadows | 1.11 ms | 0.80 ms |
    | lamps lit with their shadows | 2.71 ms | 0.86 ms |

    - So the 60 lamps' soft shadows (32 samples per lamp reaching a pixel, two to four do) cost 1.6 ms and their shading 0.2.
    - The probe pass grows with the hit lights' checks and shadow rays.
    - The roadmap's TAA is what lets soft shadows take far fewer samples.
  - **Golden images:** a new `street-night` view; every other light lab view saved anew (the probes and the sun's fitted cascades see the street, the trees on its ground are gone, the exposure follows the ground; the sun-disc sky identical value for value); the demo's matched.
  - **A delay into the dark:** trying it, the user found that switching a room's light off looked like a slow fade: the exposure began rising at once (an exponential makes its biggest step first), brightening the room while the light died away. A real eye is the other way round, nearly blind at first.
    - Into the dark it now holds for 1 s (`darkAdaptationDelaySeconds`), counting only the time past the delay so frame length does not matter.
    - A brighter frame resets the wait, so a flickering light never sets it off.
  - **Bigger discs:** the user also asked for the sun's and moon's discs at five times their real size by default (for looks; shadows keep the real size).
  - **The user's look:** the user then tuned the look by eye in the debug UI, and their settings became the defaults:
    - the exposure bias 1 (was 1.5)
    - dark adaptation 1 s (was 3)
    - the highest exposure +1 EV (was +4: dark scenes brightened by at most a stop)
    - the moon's full intensity 0.05 (was 0.25).
  - **A glow beside the doorway:** the user saw the house's outside wall glow beside the lit doorway at night, reaching further on Low than on Ultra. A camera readout on F4 (`cameraText`) gave their exact view. Experiments from it:
    - bloom off left the glow
    - bloom and probes off removed it
    - ignoring probes behind the wall left it
    - no lamp light at probe rays' hits removed it
    - no further bounces left it
    - lamp light only at hits outside the house removed it.

    So it is probe parallax, not a leak through the wall: the probes just in front of the wall see the lamp-lit jambs and room at grazing angles, which the wall itself cannot. Left for now (far less visible with the +1 EV ceiling); per-pixel short-range lighting added to the roadmap beside reflections.

#### Probes in the demo, and Lumen

- `demo-probes` (Oct 20 2026): the light probes in the demo, which until now drew with the flat night ambient only.
  - **What was added:**
    - One `ProbeCascades` round the camera.
    - The obstacle course marked static like the ground (it was in the static level already but not marked, so probes could have sat inside its blocks).
    - Each lamp field and floodlight switch one scene event (`probeEventAround` gained a form taking several points, one box round them all, so a hundred lamps restart one region, not a hundred).
  - **Fast lamps:** the user chose the flying lamps as fast lights over still ones: their bounce follows them instead of trailing for the 2 s memory.
  - **Measured:** estimated beforehand as roughly doubling the probe work while they are on, since the field covers the ground. Measured (debug exe, RTX 5080, lamp-field golden view, 120 captures) far less: the probe pass 1.65 ms instead of 1.38 (+20%), the frame 4.29 instead of 4.03; the other views' probe pass 0.28-0.75 ms.
  - **The look:**
    - Out of the lights the night is darker, since the flat ambient (0.012) came equally from every side and the probes' rays that hit the ground or the course bring back almost nothing (ambient occlusion, for free).
    - The floodlit stone and grass bounce onto the course's shaded faces and the teapot.
    - No probe buried in a moving object showed as a dark patch in any view.
  - **Golden images:** every demo view saved anew; the moon's had changed only within tolerance (54 pixels by one half-float step), since it looks at the sky.
  - **Seen while checking, not looked into yet:**
    - The light lab's `street-night` once differed from its reference in 5 pixels (up to 29 half-float steps) and the next run matched bit for bit.
    - The demo's first saved `lamp-field` was the odd one of four runs (22 pixels, up to 2 steps).
    - Both views have many lights at probe hits, so something there is not quite reproducible from run to run. `main` before this branch does the same (`street-night` passed, then failed by the same 73 pixels): the view lands on one of two pictures, not random noise.
- `lumen-research` (Oct 20 2026): the user asked how Lumen is made and tuned, and how to build something like it.
  - **Read:** Epic's SIGGRAPH 2021 and 2022 Advances talks in full (with speaker notes), their documentation and release notes, a copy of the scalability settings, and the published alternatives (GI-1.0, Brixelizer, kajiya, ReSTIR GI, radiance cascades, hash-grid caches, denoisers); written up as `docs/LUMEN.md`.
  - **What it showed:**
    - Our DDGI cascades are Lumen's outer layer (a world-space cache round the camera).
    - What we lack is its inner one, probes on the visible pixels, which Epic built after trying an irradiance field like ours and finding that irradiance computed at the probe, not the pixel, leaks and looks flat (the doorway glow found in the light lab).
    - With hardware ray queries we need neither its surface cache (our probe rays already shade their hits from the GPU scene) nor its mesh distance fields (deprecated by Epic in 5.6).
  - **The user chose:**
    - the software tracer next, as a global distance field instead of the voxel grid the roadmap named (Epic dropped voxels as leaky and slow, and AMD's Brixelizer builds the same field at run time)
    - TAA first in 6e
    - the Lumen-like layer far down the line, in phase 14, where 6e's "short-range lighting per pixel" item is folded into it, since the notes showed they are one feature.

#### The moon's phases and night vision

- `moon-phase-contrast` (Oct 22 2026): the user found the light lab's exposure right in dark rooms and under the full moon, but a moon that is not full too dark. Raising the exposure bias fixed that, but made the room with the window too bright once the lamp next door was on.
  - **The cause:** the moon is dimmed by the real phase curve (Allen's), which leaves a quarter moon at 9% of full (3.5 stops) and a fat crescent at 2.6% (5.3 stops).
    - The eye adaptation makes up only half a difference, and brightens by at most a stop, which the full moon already uses. So all of a lesser moon's shortfall showed.
    - The bias multiplies every scene, the lit room's too.
  - **Choices weighed:**
    - a dial for the lab's moon alone
    - adaptation that gets stronger in the dark (engine-wide, but it would change the dark rooms the user was happy with, and nights drift towards day)
    - or a higher exposure limit (it brightens the full moon and dark rooms too, and leaves the gap).

    The user chose the moon's dial.
  - **`LightLabNightSettings::moonPhaseContrast`:** the phase curve raised to this power, as the eye's sense of brightness, roughly a power of the light (Stevens), squeezes the stops.
    - First 0.5 (a quarter moon 1.75 stops below full); the user tried it and chose 0.25 (0.9 below).
    - The full moon is unchanged. With the contrast at 1 every golden view matched the old references bit for bit.
  - **A sky too bright:** then the user found the moon lit the sky too much against the ground, so a night looked almost like a sunrise.
    - Checked: the moon's sky is the sun's physics at the moon's light, phase included, so a moonlit sky is a daytime sky 400,000 times dimmer, orange glow round a low moon and all, as long exposures show.
    - The eye sees it otherwise: below about 3 cd/m2 the colour-blind rods take over from the cones, so the night looks grey and cool, blue relatively brighter and red darker (the Purkinje shift).
    - And with the exposure at its highest, the ground sits in the tone curve's toe.
  - **Choices weighed:**
    - night vision in tone mapping (engine-wide, the physical cause, but the dark rooms change too and the thresholds are dials until the engine has physical units)
    - or a dial for how much the moon lights the air (small, not physical, and the warm glow stays).

    The user chose night vision.
  - **Night vision:** each pixel blends towards the rods' grey, Ward Larson's scotopic luminance (1997) divided by white's, a little blue (the blue shift, 0.5), by the rods' share 1 - smoothstep(rods, cones, max(the pixel's stops, the eye's)).
    - The `max` makes a pixel go grey only when both it and the scene are dim, so lamps in a dark street and a dark corner of a sunlit room keep their colour.
    - The eye's state is the scene's mean eased like the exposure (the same hold into the dark), kept in two of `GpuEyeAdaptation`'s spare floats, so colour drains a moment after a light goes out, not at once.
  - **The thresholds by measurement:** a small decoder read the golden scene images' means in stops.
    - The moonlit field is near -8.2 EV but the sunlit room by day near -6.2: the lab's moon is about 6,700 times too bright against its sun (1/60 instead of 1/400,000), so night and a day interior are only 2 stops apart.
    - The rods are full below -9.5 EV and the cones above -7, between the two; the street at night (-4.1, its lamps) is left in colour.
  - **One smooth step:** the light lab's smoothstep and the spot cone's were copies of GLSL's smoothstep, and night vision needed a third, so all three became one `smoothStep` in `math/smooth_step.h`.
  - **Golden images:** the phase contrast changed every light lab view with the moon up (by day too: the daytime crescent's disc and its faint light in the probes), and night vision the moonlit view and the demo's night views (only their final images; the daytime views and the street at night matched). The references were saved anew.
    - Run after the GPU tests (`make gputest`), two scene images differ by a few dozen pixels (street at night, the demo's lamp field), though tone mapping cannot reach scene images; `make golden` alone matches.
    - The street's is already on the roadmap as a golden view that lands on one of two pictures.
  - **Left for later:** the user then found the sky still too bright and the ground too dark under a dim moon. Two likely causes, neither fixed:
    - a crescent moon is up only near the sun, so its glow is partly the sun's twilight
    - under any dim moon the scene sits in the tone curve's toe with the exposure at its highest, the mean set by the sky, so the ground is crushed while the sky shows (a lower contrast at night, which rods have, was proposed).

    The user chose to leave the night's look for later, with physical light units (ROADMAP phase 14), and night vision is off by default; the golden references were saved with it on, then saved again with it off (`golden-night-vision-off`: the moonlit view and the demo's night views; the flaky street-night and floodlights scene captures of that run were left at their old references).

- `docs-readability` (Oct 23 2026): the user found the docs' very long, unspaced paragraphs hard to read (single lines up to 13,029 characters in `docs/ARCHITECTURE.md`).
  - **The style:** `README.md` was rewritten first as the sample and agreed: short paragraphs with one idea each, headings, bullets with short bold lead-ins, tables where data is tabular, formulas on their own lines.
  - **The rest:** `ROADMAP.md`, `docs/BUILDLOG.md`, `docs/ENGINE_GUIDE.md`, `docs/ARCHITECTURE.md`, `docs/ILLUMINATION.md`, `docs/LUMEN.md` and `docs/RAGDOLLPLAN.md`, one agent each in parallel and one commit each. Not `CLAUDE.md`, which is for Claude only.
  - **Nothing lost:** a script compared each rewrite with the committed version and listed every number, code span, link and heading that occurs fewer times. Every drop is a stale fact corrected or a duplicate merged (`ddgi-probe-relocation` was in this log twice, word for word). Every heading text is kept, since code and docs refer to sections by name.
  - **Longest line now:** 557 characters (the guide), from 13,029.
  - **Stale facts corrected on the way**, each checked in the code: 128 probe rays by default (not 64), every light seated by default, the fast lights' rays, the adaptive memory's 8 spreads, the presets' cascades, the probe cascades' defaults, the GPU pass names and readout line, `GpuFrameData`'s and `ToneMapUniformData`'s sizes, quaternions and the release build done (the ragdoll plan), the software tracer a distance field (not a voxel grid). Bigger stale parts of `docs/ARCHITECTURE.md` are a roadmap item.
  - **`tools/package_demo.py`:** the zip's controls come from the README's Controls section, whose long rows became short rows plus notes. In the brief (anonymous) controls a heading over notes alone is now left out, and wrapped list items indent under their text.

- `architecture-stale` (Oct 24 2026): the stale parts of `docs/ARCHITECTURE.md` the readability pass had left for later, each checked in the code.
  - **Lights and shading:** the section still described the renderer before the GPU scene. Lights are now three storage buffers (1024 point, 256 spot), not a 2720-byte uniform block of 64 point and 8 spot lights. Textures come from the bindless array and the scene set, not six fragment sampler slots. Handedness is in each object's instance, not pushed per draw, and the shading lives in `scene_surface.glsl`.
  - **Golden views:** the light lab has nine, with `street-night`. The two "all twelve golden images" are kept as what was checked then.
  - **Smaller fixes:** the demo's hover readouts are on lines 11 to 13, after its system timings on `firstGameReadoutLine` (10). The sentence on the order of `Application`'s members moved to its section. The tone mapping pass is `drawToneMap`, and the raw shadow atlas is scene set 1 binding 8.
  - **Also:** the engine guide's "Covers" list, a run-on sentence cut into bullets ("so all of 6c"), became one line per phase (`guide-covers-list`).

- `taa` (Oct 23 2026): temporal anti-aliasing, 6e's first item. Each pixel showed the scene at one point, so edges stepped and thin things (the light lab's fence) shimmered as the camera moved.
  - **How:** the main pass is jittered on a 16-frame Halton (2, 3) cycle, and each pixel keeps an exponential average of what it saw (each new frame weighted 0.1), reprojected to where its surface was last frame and clipped into the box of its 3x3 neighbours' colours (mean plus or minus one standard deviation, in YCoCg), each sample weighed by bloom's Karis weight, the history read with Catmull-Rom.
  - **Motion, decided:** a separate velocity pass that redraws only the objects that moved, as UE4 does, rather than a second colour output from the main pass. The sky and stars are drawn inside the main pass's rendering block, and every pipeline there must have the same colour attachments, so a velocity output would have changed all of them. Still things and the sky are reprojected from depth and the two cameras in the resolve instead.
  - **Last frame's matrices:** `MotionHistory` (owned by `Application`, render-only) remembers each model part's matrix by entity and part, so each `DrawItem` carries `previousModel`; the renderer keeps last frame's camera.
  - **The result goes back over the scene image** with a copy, so eye adaptation, bloom, tone mapping and captures read the smoothed picture without changes of their own.
  - **Reused:** bloom's `karisWeight` for the blend, and a general `inverse` added to `math/mat4` (Gauss-Jordan in doubles) for the reprojection, which is exactly the identity for a still camera.
  - **Captures** draw 32 frames first (two jitter cycles), restarting the history and the cycle, so golden images repeat exactly. Golden images were not run or saved in this branch (only on request): every view's final image will change.
  - **Checked:** the tests (debug and release), and the validation layers in the light lab and the demo (no messages). The GPU test cases for `taa.glsl` are written and built, not run (only on request).

- `dlss-model` (Oct 23 2026): the user saw a trail at the edge of the flashlight's beam with DLSS, not with FSR or TAA.
  - **Why:** the flashlight moves with the camera, so its beam stays put on screen while the surfaces slide under it; the motion vectors rightly carry each surface's history along, old beam edge included, and only a check that the history no longer fits throws it away. TAA's clip into the neighbours' colour box does that; DLSS's transformer model K (its default for DLAA) holds history more strongly.
  - **First step, chosen as the cheapest:** a setting for the DLSS model in every mode (`UpscalingSettings::dlssModel`: Default, J, K, L, M), to judge by eye. NVIDIA's notes: J ghosts a little less than K but flickers more, L less than J and K but costs more, M like L, closer to J and K in speed.
  - **The user's verdict:** M worked best, so it is the default.
  - **If it ever trails again:** a mask telling DLSS to favour this frame where fast lights shine (Streamline's `BiasCurrentColor`; FSR's reactive mask is the same idea).
  - **Checked:** the tests (debug and release; tests first), and the light lab with the validation layers on each model (no messages).

- `taa-choice` (Oct 23 2026): the user asked for TAA to switch off by itself with FSR or DLSS on.
  - **What ran already:** with FSR or DLSS active the engine's resolve was already skipped; the jitter and velocity pass still run, as both need them. But the TAA checkbox was a master switch (off turned FSR and DLSS off too), and F5 showed TAA on beside DLSS.
  - **Chosen with the user:** one choice instead of two settings, "Anti-aliasing" in the Upscaling group: Off, TAA, FSR 3.1 or DLSS (`Upscaler::Off` added, `TaaSettings::enabled` removed, `temporalAntiAliasing`). Keeping both and decoupling them was weighed: a smaller change, but two settings for one thing. TAA's tuning moved to its own group, TAA. The F4 readout says NO ANTI-ALIASING when off.
  - **Checked:** the tests (debug and release; tests first), and the light lab with the validation layers at each of Off, TAA, FSR and DLSS (no messages but FSR's first-frame note).

- `ultra-dlss-defaults` (Oct 23 2026): the user asked for DLSS at Native (DLAA) and the Ultra preset as the new defaults, with sharpness 0.8.
  - **Ultra is now the defaults** (1024 probe rays, near fast lights too, no sharing between probes), and High is listed as Ultra with 256 rays and 0.1 sharing, so retuning a default retunes Ultra.
  - **DLSS at render scale 1:** where DLSS is missing (no RTX GPU, or Streamline's DLLs not fetched) it falls back to TAA as before.
  - **Sharpness 0.8 is FSR's** (RCAS): DLSS and TAA have no sharpening of their own in the engine, so it shows only when FSR is picked.
  - **Cost, not measured here:** the default probe work is about four times High's (`probe-ray-presets`), with 201 MB of ray buffers.
  - **Checked:** the tests (debug and release), the light lab and the demo with the validation layers (no messages, DLSS active). Golden views change again (still stale since `taa`).

- `toksvig` (Oct 23 2026): normal map filtering, 6e's next item. A smaller mip averages the bumps under each texel into one direction, so a distant bumpy surface shaded like a smooth mirror whose highlight broke into flickering dots.
  - **How (Toksvig 2005):** the average of unit directions is shorter than 1 the more they disagree. Its spread (1 - L) / L widens a Phong lobe as 1/s' = 1/s + spread; with GGX's alpha² ≈ 2/s it adds 2 · spread to alpha² (alpha = roughness²). For small tilts that is about the bumps' mean squared slope: the variances of the material's lobe and the bumps add, like two blurs one after the other.
  - **The mips already kept the length:** they are blitted on the GPU with a linear filter and never renormalised, and decoding (2c - 1) is linear, so averaging texels averages directions. No loader change was needed.
  - **8-bit rounding:** a full-size texel is short of 1 by up to sqrt(3)/255 from rounding alone, which would have made every smooth normal-mapped surface noticeably rougher close up (roughness 0.045 to about 0.3). That much shortening counts as none (`NORMAL_TOKSVIG_QUANTIZATION_SLACK`, in the new `normal_mapping_shared.h`), so close up the material's roughness comes back exactly.
  - **The scale:** glTF's normal scale stretches the slopes, so the spread is multiplied by its square; scale 0 (no normal map) adds nothing.
  - **Weighed:** LEAN/CLEAN maps baked at load (more exact, more memory and loader work) and geometric specular anti-aliasing (the same for curved geometry, from the normal's change across the pixel; a possible later step). Toksvig costs a few instructions a pixel.
  - **Setting:** `RenderSettings::normalMapFiltering`, on by default (F5, Passes), off to compare. The normal map is now read once, before the roughness and outside the normal's branch.
  - **Checked:** the tests (debug and release; tests first), the demo and the light lab with the validation layers (no messages). The GPU test case for `toksvigRoughness` is written and built, not run (only on request). Golden images not run (still stale since `taa`); views with normal maps change.

- `dlss` (Oct 23 2026): the last step of 6e's DLSS and FSR item: NVIDIA's DLSS as a third upscaler, through Streamline.
  - **NGX or Streamline:** DLSS's own SDK links static libraries built with Microsoft's compiler, which GCC cannot (they call into Microsoft's C++ runtime, and the licence forbids looking inside them). Streamline, NVIDIA's recommended path, is a DLL with `extern "C"` functions and plain structs, loaded as FSR is.
  - **The trial first** (a scratch program, not committed): load the interposer, `slInit` with manual hooking, DLSS's Vulkan requirements, an instance and device made with them, `slSetVulkanInfo`, `slIsFeatureSupported` (yes on the RTX 5080), and `slDLSSGetOptimalSettings` at 2560 x 1440, which came back right in every mode (Quality 1707 x 960, Balanced 1485 x 835, Performance 1280 x 720, Ultra performance 853 x 480): structs pass both ways. `slInit` after `slShutdown` in the same process worked too, which the golden images need (one renderer per game).
  - **Licence, decided with the user:** DLSS (`nvngx_dlss.dll`, 59 MB) is under the NVIDIA RTX SDK licence, which lets an application ship it but not hand it out alone, and asks for NVIDIA's credit; GitHub warns at 50 MB. So its DLLs are fetched by `tools/fetch_streamline.py` (pinned, SHA-256 checked) into a git-ignored folder, while Streamline's MIT headers are committed so the engine always builds. The packaged zip carries them with their licences and a DLSS credit.
  - **Read in NVIDIA's guide before the tests:** its reference motion vector shader computes our quantity (NDC now minus then) and turns it into pixels from now to then, so Streamline's normalised scale for our velocity image is (-0.5, +0.5); the jitter is in render pixels "in the same coordinate and direction system as the motion vectors"; the mip bias and phase counts are FSR's; Streamline's matrices are DirectX's row vectors, ours transposed.
  - **Found when it ran, each fixed:** per-frame tagging needs `eUseFrameBasedResourceTagging`; Streamline creates private data slots (`privateData`, required by Vulkan 1.3, now on); tagging with `eOnlyValidNow` made it copy every image (validation errors: our images are not copy sources), `eValidUntilPresent` reads them in place, as NVIDIA advises first; DLSS clears its output (`TRANSFER_DST`); and manual hooking still needs every present through Streamline's `vkQueuePresentKHR` for its bookkeeping. A refused evaluation now falls back to TAA instead of stopping the game.
  - **Render sizes:** DLSS works in its modes, each taking a range of render sizes, so a custom scale draws in the nearest mode, clamped into its range.
  - **Checked:** the tests (debug and release; tests first for the upscaler choice, the nearest mode, the clamp, the matrices and the motion scale), the light lab with the validation layers at DLAA, Quality and Ultra performance and the demo at Performance (no messages), the game without the interposer (one log line, TAA), and the package. The picture is for the user to judge, the jitter's sign above all (a still scene would shimmer if it were flipped).

- `probe-ray-presets` (Oct 23 2026): the user asked for 256 probe rays on High and 1024 on Ultra (they were 128 and 256), for probes near fast lights too. High is the defaults, so the default is now 256 rays a probe. Low and Medium stay at 32 and 64.
  - **Cost, not measured here:** tracing and updating go with the rays, so High's probe work about doubles and Ultra's is about four times the old Ultra's. The ray buffers hold rays only for the frame's traced probes (at most 8,192): 50 MB at 256, 201 MB at 1024.
  - **Checked:** the tests (debug and release), and the light lab with the validation layers at 256 and at 1024 rays (no messages). Golden views with probes change again (still stale since `taa`).

- `fsr` (Oct 23 2026): the second step of 6e's DLSS and FSR item: AMD's FSR 3.1 as an upscaler beside the engine's own TAA, with full-screen motion vectors for it.
  - **Which FSR:** SDK v1.1.4 (May 2025, FSR 3.1.4) is the last line with a Vulkan backend; SDK 2.x (FSR 4, RX 9000 only) is DirectX 12 only. MIT licence, and a prebuilt DLL signed by AMD, vendored in `external/fidelityfx` (the user chose to commit it rather than download it).
  - **MinGW:** the FidelityFX API is five C functions behind typed, chained structs (like Vulkan's `pNext`), so the DLL, built with MSVC, is loaded with `SDL_LoadObject` and called through function pointers; C++ ABIs differ between compilers, C's do not on 64-bit Windows. Nothing links against AMD's `.lib`, and the game runs without the DLL.
  - **Read in AMD's docs and source before writing tests:** motion vectors run from this frame's pixel to last frame's (`motionVectorScale` = (-w/2, +h/2) for our NDC differences), the jitter's sign matches `jitteredClip`, the phase count is ceil(8 n²), the mip bias log2(scale) - 1, auto exposure recommended (so the engine's exposure is not reordered to come before it), and reversed-Z non-infinite depth matches our projection exactly.
  - **Found when it ran:** FSR's backend picks its shader paths from the extensions the GPU lists, not those enabled. It called `vkGetBufferMemoryRequirements2KHR`, which a device only returns with `VK_KHR_get_memory_requirements2` on, and crashed through a null pointer (found with gdb); the device now enables that and `VK_KHR_dedicated_allocation`, plus 16-bit floats and integers and storage reads without a format. Its debug checks wanted near and far swapped for inverted depth.
  - **AMD's bug, filtered (the user's choice):** its shader declares the luma history image `rgba8` while FSR makes it RGBA16F (in `ffx_fsr3upscaler_callbacks_glsl.h`), a validation warning every frame. The one message is skipped by name. Rebuilding the DLL with the fix was weighed and set aside: CMake and Visual Studio, an unsigned DLL to maintain.
  - **Upscaling modes** (the user asked after trying it): FSR's and DLSS's named modes as a combo beside the quality preset, setting the render scale (Quality 1/1.5, Balanced 1/1.7, Performance 1/2, Ultra performance 1/3), Custom between them; the scale stays the one stored setting.
  - **Checked:** the tests (debug and release), the light lab with the validation layers at FSR 1, 0.67 and 0.5 and the demo at 0.67 (nothing but FSR's first-frame note that the frame time is under 1 ms), and the packaged zip's DLL check. The GPU test cases for `cameraMotion` and the jitter's cycle length are written and built, not run. The picture itself is for the user to judge.

- `render-scale` (Oct 23 2026): the first step of 6e's DLSS and FSR item: drawing the scene smaller than the screen and letting TAA build the full-size picture (TAAU), so the passes that cost per pixel can shrink. DLSS and FSR need the same split (a render size and an output size), and it gives every GPU an upscaler of its own.
  - **Order, decided with the user:** render scale and TAAU first (no vendor code), then FSR 3.1 (open source, every GPU, a C interface in DLLs), then DLSS after a trial of NVIDIA's SDK with MinGW, which it is not built for.
  - **How:** the history moved to the output's size and the scene image, depth and velocity to the render size. Each output pixel rebuilds this frame's colour from the 3x3 render pixels round it, each weighed by how far its jittered sample landed (Karis's Gaussian fit of Blackman-Harris, exp(-2.29 d²)), and takes the new frame's share times the nearest sample's weight, so a pixel no sample landed near keeps its history. Depth and motion come from the nearest of the 3x3 to the camera (depth dilation). A mip bias of log2 of the scale keeps textures at the output's sharpness.
  - **No copy back:** the resolve's result can no longer go back over the scene image (a different size), so the passes after it read the finished picture (`finishedScene`) instead, which saves a full-screen copy each frame.
  - **At full size the picture changes a little too:** this frame's colour is now the reconstruction, not the pixel's own texel, which puts each sample back where it really was (less wobble) but is a little softer (a Gaussian of 0.47 px against the jitter's box of 0.29 px), and the average runs a few frames longer. A sharpening pass (FSR's RCAS) is the usual partner and comes with FSR.
  - **Left for step 2:** full-screen motion vectors (the camera's motion for still pixels): the resolve reprojects them from depth itself, but FSR and DLSS need them in the image.
  - **Checked:** the tests (debug and release), and the light lab with the validation layers at scale 1 and 0.5 (no messages). The GPU test cases for the new `taa.glsl` functions are written and built, not run (only on request); golden images were not run (still stale since `taa`).

- `ddgi-more-rays` (Oct 23 2026): the user asked for up to 1024 rays a probe (it was 256). Two things stood in the way.
  - **Shared memory:** the irradiance update held all of a probe's rays in workgroup shared memory (direction, light and fast light, about 48 bytes a ray): about 48 KB at 1024, NVIDIA's limit. It now streams them through 256 at a time, each texel adding a chunk into its sums in ray order, so the sums are the same as before for any count.
  - **Memory:** the ray buffers had room for every probe (about 45,000), though a frame traces 3,000 to 6,000: 1024 rays would have taken 1.1 GB. They now hold rays only for the frame's list of traced probes (`chooseTracedProbes`), each at its place in the list, and the trace and update work through the list (which also ends the trace's idle invocations for untraced probes).
  - **The cap:** before the first relocation records every probe counts as vigilant, and so with probe states off, so the list is capped at 8,192 probes; past it a window of 8,192 moves on each frame and they take turns. With the cap the buffers take 25 MB at 128 rays (was 138 MB), 50 MB at 256 (was 276 MB) and 201 MB at 1024.
  - **Options weighed with the user:** raising the limit to 512 only (small, but 553 MB of rays), this (chosen), or leaving the rays and tuning the memory or sharing for the same calm. Expected cost, not measured: tracing grows with the rays, so 1024 at 60 updates a second is roughly 2 to 4.5 ms on the RTX 5080 against 0.22 to 0.56 ms at 128; the ray budget still divides the rays above 60 frames a second.
  - **Checked:** the tests (debug and release), a new probe-pass GPU case at 600 rays (three chunks, written and built, not run), and the validation layers in the light lab at 1024 rays (no messages). Golden images not run.

- `ssr` (Oct 23 2026): screen-space reflections, the first half of 6e's reflections item.
  - **Order, changed with the user:** the roadmap had parallax-corrected reflection probes first. The light probes' mirror-direction stand-in already covers what SSR misses, so SSR first gives sharp reflections soonest; probes come next, placed automatically round the camera and corrected by their own depth, since hand-placed boxes would go against emergent lighting.
  - **Blur, chosen with the user:** one mirror ray per pixel and a colour pyramid read as wide as the reflected cone, rather than random rays from the GGX lobe averaged over frames: stable with no denoiser, and no new risk of DLSS trailing (it trailed at the flashlight's edge). Stochastic rays fit phase 14's ray-traced reflections, which need the same denoising.
  - **A forward renderer's surfaces:** the main pass writes three small targets (normal, specular albedo and roughness, its stand-in) behind a specialization constant, 12 bytes a pixel (44 MB at 1440p). Recomputing them in the pass from depth was weighed and dropped: it would re-read the materials and duplicate the shading. The stand-in is subtracted rather than left out of the main pass, so where the reflection is not trusted the picture is the main pass's to the bit.
  - **The sky in its own block:** every pipeline inside one rendering block must name the same targets, so with the surfaces the sky and stars draw after the main pass in a second block (the reason `taa` drew motion in a pass of its own).
  - **Depth pyramid sizes:** a mip chain rounds each level down, so an odd level's last texel is folded into the last cell above (three texels in place of two), and every texel has a cell.
  - **Reflections need anti-aliasing on:** they reflect last frame's finished picture, which with anti-aliasing off is the scene image this frame draws over.
  - **Light lab:** a polished stone slab (roughness 0.08) and a chrome ball (0.05) north-east of the start; golden views from the start see them.
  - **Checked:** the tests (debug and release; tests first, a synthetic scene's floor reflecting a wall at the height the mirror image predicts, a sky miss, and a panel the ray passes behind, skipped with a thin thickness and hit with a thick one), and the light lab with the validation layers (no messages; a screenshot showed the ball mirrored in the slab). The GPU test cases for `screen_reflections.glsl` are written and built, not run (only on request); golden images not run (still stale since `taa`). Cost not measured yet.

- `reflection-probe` (Oct 23 2026): the second half of 6e's reflections item: a reflection probe for what the screen cannot reflect.
  - **Placement, chosen with the user:** automatic, no boxes. One probe at the camera, captured every frame, against a lattice of eight round it. The user asked which looks best: the lattice sees round corners the camera's point cannot, but its probes would take turns (reflections of moving lights trailing) and blend with seams; the camera probe is always fresh. The lattice stays an addition if gaps show.
  - **Parallax from its own depth:** each texel keeps how far its surface is, and a fixed-point search (distance impostors) finds where a pixel's mirror ray meets what the probe saw. Its confidence falls where the two distances never agree, which is exactly where the probe could not see the hit.
  - **The sun past its cascades:** captures see all round the camera, the cascades only its view, and the shader lit everything past them. On GPUs with ray queries the capture traces the sun's shadow there; without them it stays lit (a coarse cascade all round the camera is the fallback, not built).
  - **Shader variants instead of constants:** the validation layers warned that pipelines without the reflection surfaces' targets still wrote them, a specialization constant switching the writes off notwithstanding. `triangle.frag` became `scene_surface.glsl` with small entry files defining what each variant has: the plain main pass (as before reflections), the main pass with surfaces, and the two captures (`#version 460` for ray queries).
  - **Reused:** the BRDF table's GGX sampling (`ggxSampleHalfway`, moved public), texture uploads' mipmaps (`generateMipmaps`, now shared), the light probes' sun shadow ray (`reachesLight`, moved into `ray_scene.glsl`), the compute passes' descriptor helper (`bindComputeSet`, now in `vulkan_compute`), and `linearShadowDepth` for the distances. `VulkanImage` gained layers and cubes; barriers now cover every layer.
  - **Presets:** the probe's faces are 256 texels on Ultra (the default), 128 on High, 64 on Medium; off with reflections on Low.
  - **Checked:** the tests (debug and release; tests first: the cube addressing against Vulkan's table, the faces' views and mirror winding, the sky rays, the search against a sphere, a wall and sky, and an unseen world, the prefilter's levels, uniform light and light from above), the light lab with the validation layers (no messages; a screenshot showed the ball mirroring the trees, towers and sky round it). The GPU test cases are written and built, not run; golden images not run. Cost not measured yet (CUBE in the F4 readout).

- `mirror-and-fps` (Oct 23 2026): the user asked for a mirror in the sunlit room on the wall opposite the doorway (silvered glass: metallic, roughness 0, 1.4 by 1.8 m, `labMirror`, with the polished slab in the layout's `reflectors`) and for the frame cap's default to drop from 240 to 120 fps (`EngineSettings::targetFPS`). Checked: the tests (debug and release; the mirror's placement tested first) and the validation layers (no messages).

- `reflection-probe` fix (Oct 23 2026): the user's shots of the new mirror showed the probe's parallax search going wrong: the doorway pinched towards the floor, streaks along its top and bottom, and from an angle curved cuts and pieces of the back room through the wall. The fixed-point search settled on later roots of |P + R t| = D (through the doorway, behind the wall) and trusted them, since they matched perfectly. Replaced by a march to the first crossing behind what the probe saw (32 geometric steps from 2 cm, 8 halvings, as the distance impostors paper does), trusted by how far the probe's distances either side of the crossing agree. Tests first: a wall with a doorway, rays beside it, through it and across its edge. Checked: the tests (debug and release) and the validation layers.

- `reflection-probe` follow-up (Oct 23 2026): backing away and standing in the doorway still looked wrong. From the doorway the probe sees the wall round it edge-on, a sliver on the cube, and the march's metre-long steps passed over it into the room beyond. Three options were laid out (a distance pyramid for every GPU, ray-traced mirror rays on RT GPUs, both); the user chose both.
  - **Distance pyramid:** each level the nearest of the four below; a step sweeping more than a texel of the probe is taken only if nothing in its cone of directions is nearer than the ray (two reads at a level four times the cone's width), shorter steps check the ray's point. A fixed 64-step budget ran out on a floor seen at a slant from the probe (the cone test kept refusing); steps of a texel or less now check the point directly and the budget is 128.
  - **Ray-traced reflections:** `rayTraced` (off by default, the rule for ray-traced features), the hit lit from the probe's picture where the probe saw it, else as probe rays light their hits; the probe trace's hit lighting moved into `ray_hit_lighting.glsl` for both, and compute pipelines can now read shared sets before their own.
  - **Found on the way:** the pass record's probe origin and cube sizes were in a different order in the shader than in C++ since the pyramid commit (the compiler caught it when the ray variant added a field).
  - **The reflected sun patch's stair steps** are the probe's texels (about a third of a degree at 256), not hard shadows: the sun's penumbra there is under a centimetre. A 512 probe or ray-traced reflections with their own shading would sharpen them; left as is.
  - **Checked:** the tests (debug and release; the doorway seen edge-on and the floor beside it tested first), the validation layers with ray-traced reflections off and on (no messages).

- `reflection-probe`, third round (Oct 23 2026): the user found more: with ray tracing a wedge beside the doorway (which went away with a thinner thickness), without it a ragged doorway, and on the slab blocky tower reflections and stripes. Screenshots alone were too slow to work from, so a temporary harness (never committed) captured exact views through the golden image path with debug switches (probe only, screen-space only, confidence, ray-traced), and each fix was checked on them.
  - **The wedge:** screen-space reflections of rays heading back towards the camera (a mirror seen head-on) passed just behind the window's side wall on screen and counted as hits. Now faded out (cosine 0.3 to 0.7 with the way back to the camera); such mirrors are the probe's or ray tracing's.
  - **Far towers blocky:** screen-space reflections faded out at 100 m, leaving the 80-320 m towers to the probe, whose edge test then rejected their silhouettes. The reach is now 1000 m, and a crossing is also trusted where the probe looks at it along the ray's own line (within a texel): far things seen from near the probe.
  - **Stripes:** rays rising off the slab caught on the ground's coarse texels just past their start. They now start lifted off the surface by two of the probe's texels, more where the probe sees the surface at a slant; rays reaching the far plane, or lined up with the probe's view, read the probe along the ray.
  - **Floors seen at a slant:** neighbouring texels differ a lot there, so the edge test (exact texels either side) rejected real floors. Distances are now read blended between texels for finding the crossing, and an edge is told by how far the four blended texels spread (textureGather).
  - **Bands along the floor and ceiling:** the cone-checked march crawled near them and ran out of steps (1024 steps removed them, too slow). Replaced by a cell-by-cell walk of the distances' pyramid, as screen-space reflections walk theirs: a texel's edges are planes through the probe, so where a ray leaves a cell is exact; 64 steps suffice.
  - **Found on the way:** with the probe switched off its images were never made, and the pass bound them anyway (a validation error); they are now made whenever reflections run.
  - **Checked:** the tests (debug and release; tests first for each: the slanted floor, the far post's silhouette, rays rising off the slab, cell exits, the edge test), the four debug views without ray tracing (the slab, the mirror close, at an angle, from the doorway) and two with it, and the validation layers. A few small ragged spots stay where the doorway's jambs meet the floor seen from inside the doorway without ray tracing: the probe's honest limit there.

- `mirrored-lights` (Oct 23 2026): the user found that a flashlight shone into the light lab's mirror did not light the spot its reflection points at. Of the options explained (the image light for flat mirrors, manifold next event estimation, light tracing into the probes, caustic maps), the user asked for the general and physically correct one: manifold next event estimation (Hanika, Droske, Fascione 2015), on ray-tracing GPUs, off by default.
  - **How:** per pixel and pair of a fast light and a smooth shiny surface (found from materials), Newton's method on the surface for the law of reflection, each guess put back onto the surface by a ray from the pixel; the brightness from the constraint's Jacobian (the spread area dx² dL² |det J| / cos²θ in place of distance² in the lights' falloff). A pass between the depth prepass and the main pass leaves the light and its direction in two images the main pass adds as one more light.
  - **Checked:** a flat mirror gives exactly the image light, and the ball's spread area agreed within 0.3% with 2.25 million light rays reflected off an exact sphere and counted where they land (a flat mirror would have been 14 times brighter there). In the light lab at night the flashlight's beam, wider than the mirror, comes back as the mirror's own shape on the floor and the window wall.
  - **Measured and fixed:** 3.2 ms at 1440p at first: pixels whose point of reflection lies off the mirror's edge crept towards it through Newton's halvings, up to 40 rays each. On a flat surface the first (image) step is exact, so a step falling off now ends the search: 0.68 ms.
  - **Left for later:** the bounce into the light probes, the lit spot seen in reflections, the image light without ray tracing, rough surfaces.
- `mirrored-light-bounce` (Oct 23 2026): the mirrored lights' follow-ups, the user asking for all but the image light without ray tracing. Rough mirrors by a Gaussian footprint (option A, the user's choice over specular manifold sampling (Zeltner 2020): unbiased but noisy and costlier).
  - **One search.** The search and its rays moved into `mirrored_light_search.glsl` and the pairs into scene set 1, so the pass, the probes, the reflection probe's traced capture and traced reflections all run the same code over the same pairs.
  - **Metals' matte colour.** The rays' hits took the base colour whole, so the mirror looked like a white wall and the chrome ball like plaster to the probes and traced reflections: now base × (1 − metallic), as the main pass. Changes the light near metals.
  - **Rough mirrors.** With Gaussian facet slopes the light off a mirror is a Gaussian integral over a footprint round the point of reflection; on a mirror bigger than the footprint it is exactly the sharp answer, so roughness only softens edges. Sampled by importance, the footprint held to the mirror's size so a focus stays finite. Checked against the exact sum over a finite rough mirror's Beckmann facets across its reflected edge: within 3%. The roughness limit went from 0.1 to 0.25, fading out by 0.3.
  - **Two bugs found in the picture.** The flashlight's `sourceRadius` (4.5 m, the falloff's dial) had gone in as the bulb's size and spread the footprint over the whole mirror, lighting the floor evenly: the footprint now takes `emitterRadius` (2.5 cm), as soft shadows do. And a thin mirror's rim: where the flat step landed on the box's side face the search failed, a dark line along every soft edge; landing outside the mirror's plane now counts as off the mirror.
  - **Measured and moved.** Calling the search in the probe trace slowed every probe ray, mirrored lights off or not: 5.7 to 8.3 ms at 1440p on Ultra (the shader's registers, not the work). The bounce became its own pass over the trace's rays near fast lights: off is back to 5.9 ms, on costs about 2 ms with the flashlight filling a room (Ultra's 1024 fast rays a probe), so it has its own setting (`bounce`). The pass: 1.2 ms sharp, 2.3 ms with a rough mirror; the traced capture 0.35 ms more.
  - **Checked:** the tests (debug and release), and the light lab with the validation layers (no messages): the sharp mirror's quadrilateral on the floor and window wall, softening with roughness, and its bounce lighting the room. The GPU test cases for the new functions are written and built, not run (only on request); golden images not run (stale since `taa`, and the matte colour fix changes the probes near metals).
  - **A rough mirror in the lab.** Nothing in the light lab sat between roughness 0.1 and 0.25, so the footprint showed only in debug captures: a brushed steel panel (roughness 0.2, `labSteelPanel`) now hangs beside the sunlit room's mirror, as big, so one flashlight sweep compares the sharp and the soft reflected spot. Left for the user to judge in the game.
- `gtao` (Oct 23 2026): ambient occlusion, the next item in 6e, the user asking to check the picture by hand (CPU tests yes; no captures, timings or GPU tests).
  - **How:** ground-truth ambient occlusion (Jimenez et al. 2016) from the depth prepass: slices through each pixel round the way to the camera, horizons found walking both ways through the depth image, the cosine-weighted open arc between them in closed form, averaged over slices turned each frame for TAA (a depth-aware blur without anti-aliasing). Radius 0.5 m, about the densest probes' spacing, so the probes keep what is bigger. Jimenez's multiple-bounce fit and Lagarde's specular occlusion; only the light from all around is dimmed. On by default; off on Low, fewer slices and steps on Medium and High.
  - **Found by the tests:** the first normal from depth took the nearer neighbour each way, which at a wall's foot is the wall's pixel (the floor below is foreshortened): the floor got the wall's normal, and the long-radius occlusion there was 0.62 against the 0.5 an infinite wall gives. The neighbour whose next one carries on in line (a straight line of pixels on a plane is straight in space) gives 0.506.
  - **Not checked here:** the picture and the cost, left to the user; the GPU tests are written and built, not run; golden images not run (AO is on by default, so every view darkens in its creases).
- `ultra-rt-defaults` (Oct 23 2026): the user's choice: Ultra (the defaults) traces 256 probe rays again (High's, which keeps them too), and turns on the ray-traced features, ray-traced reflections and mirrored lights (with their bounce and rough mirrors), for GPUs with ray queries; below Ultra they are off. High is now Ultra without them, with a little probe sharing, a smaller reflection probe and lighter ambient occlusion. The 1024 rays went to pay for them: the mirrored lights' bounce, for one, cost about 2 ms at 1024 fast rays a probe and about a quarter of that at 256. Not run in the game (the user checks it); golden images not run (they now draw with the ray-traced features on, on GPUs with ray queries).
- `gltf-occlusion-emissive` (Oct 23 2026): glTF's occlusion and emissive textures, the next item in 6e after mirrored lights and GTAO.
  - **Combining the two occlusions:** the material's baked occlusion (glTF: 1 + strength (texel - 1)) and the screen's GTAO both estimate the share of the sky a point sees, so the main pass takes the more closed of the two (Filament's choice), not the product, which would darken a crease both see twice. Then the same multiple-bounce and specular-occlusion fits. The baked one also applies in the reflection probe's capture, being the surface's own, and with GTAO off or on Low. Ray hits read the emissive texture (so a glowing pattern lights the probes) but not the occlusion (a texture read per ray for little on lighting that coarse).
  - **Packing:** `GpuMaterial` grew from 64 to 80 bytes: the emissive slot took the free fourth texture slot, the occlusion slot a new `moreTextures` (the rest kept for later textures), the strength `surface.w`, packed as 0 without a texture like the normal scale, so materials without one draw exactly as before.
  - **Model:** the user first chose Khronos's DamagedHelmet, but its licence carries a non-commercial clause (CC BY-NC 4.0, from the 2016 original) alongside CC BY, which would bind the repo and the demo zip; the user picked WaterBottle instead (Microsoft, CC0, 9 MB), now in the demo's gallery. Its occlusion is packed with roughness and metallic in one image (ORM), named by two texture slots.
  - **Found on the way:** the loader copied an embedded image once per texture slot naming it, so the `AssetManager` (which caches embedded images by buffer) decoded and uploaded an ORM image twice. Each embedded image is now copied once per load (`GltfImages`).
  - **Checked:** the tests (debug and release, tests first: the loader's slots, strength and its check, the bottle's shared image loaded once, encodings, packing, `surfaceVisibility`). Not checked here: the picture, left to the user; the GPU test case is written and built, not run; golden images not run (the demo's gallery gained the bottle, so views that show it change).
- `alpha-mask` (Oct 24 2026): the first half of 6e's alpha item, cut-outs (glTF's alpha mask), the user choosing mask before blend, coverage-preserving mips over hashed alpha, and (for the blend half) sorting per object over weighted blended OIT, after the pros and cons of each.
  - **Double-sided too:** nearly every cut-out asset (and all of Khronos's alpha test materials) is double-sided, which the engine ignored, so the user had it included here: no culling for those batches (Vulkan 1.3's dynamic cull mode, so no extra pipelines), the back shaded as the front's mirror image, and rays counting either side as a front (else probe relocation took a fence's back for the inside of a wall).
  - **Mips:** Castano's coverage-preserving mips, built on the CPU when a cut-out's colour texture loads (a 256-bin alpha histogram makes the bisection cheap); the GPU's blits stay for everything else. The scale nearest 1 among those giving the nearest coverage keeps solid textures solid.
  - **Passes:** draws are batched by style, cut-outs last, and each pass picks a pipeline per batch: the prepass and shadows gain a fragment shader only for cut-outs, the main pass after the prepass needs no discard at all (the holes have no depth for its EQUAL test), and a specialization constant adds the discard where it is needed, so other pipelines keep early depth tests.
  - **Rays:** cut-outs' ray instances are not opaque, and every ray query now runs through one loop that confirms a candidate only on a solid texel; opaque instances still commit at once.
  - **Seen:** the light lab's fence gained a chain-link (texture made in code, no asset), and the demo's gallery Khronos's AlphaBlendModeTest (CC BY 4.0, Ed Mackey/AGI), whose blended row draws opaque until the blend half.
  - **Found on the way:** MSYS `sed -i` silently rewrote CRLF files as LF (and its grep hides the CRs), so files edited that way turned LF against CLAUDE.md's CRLF rule; they were converted back, and line endings are now checked with perl after edits.
  - **Checked:** the tests (debug and release, tests first). Not checked here: the picture and the validation layers, left to the user; the GPU test case is written and built, not run; golden images not run (the gallery and the lab's fence changed).
- `glass` (Oct 24 2026): the second half of 6e's alpha item. Of the three ways laid out (glTF coverage only, coverage with the reflection kept, or coverage plus thin glass), the user chose the third, and the upscalers' masks for later.
  - **Why not coverage alone:** glTF's BLEND scales everything about the surface by alpha, its reflection too, so clear glass (alpha near 0) loses nearly all its reflection and tints nothing. Glass covers the whole pixel: Fresnel reflects about 4% head-on and nearly all at grazing angles, and the rest passes through tinted. KHR_materials_transmission says so in glTF; BLEND keeps its coverage meaning, as glTF's viewer draws it.
  - **Per-channel tint:** one alpha cannot make red glass pass red, so the pass blends with dual-source blending (the shader's second colour multiplies the picture per channel), on every desktop GPU; a grey share where it is missing.
  - **Order:** sorted per object, far to near, double-sided objects drawn backs first then fronts, the user's choice over weighted blended OIT (which blends colours of stacked layers together and needs tuning across the lab's centimetres to 300 m).
  - **Reflections on glass:** glass is not in the depth image, so screen-space reflections cannot see it; it reads this frame's reflection probe by direction instead, at its roughness's level.
  - **Left out for now:** glass casts no shadow (the sun through the lab's window stays untinted), stops no ray, is not in the probe's capture and writes no motion. Its ray mask is 0, and mirrored lights skip it, since their rays could never land on it.
  - **Found on the way:** the renderer's check that a material's textures were uploaded only knew the colour, metallic-roughness and normal textures; it now checks occlusion, emissive and transmission too. And the perl delimiter trap in the tooling notes struck again: a substitution put a stray line at the top of the renderer, caught by the next grep and removed.
  - **Checked:** the tests (debug and release, tests first). Not checked here: the picture and the validation layers, left to the user; the GPU test case is written and built, not run; golden images not run (the lab's window and the gallery's blended row changed).
- `transparency-motion` (Oct 24 2026): the first of the three parts transparency's leftovers were split into (the masks for anti-aliasing, tinted shadows, refraction), in the order recommended and accepted.
  - **The problem:** anti-aliasing reprojects each pixel by the motion of what the depth image holds, and glass is not in it, so moving glass, and reflections sliding over still glass, would smear.
  - **Measured rather than drawn:** the mask was first planned as a second, unlit draw of the glass (its coverage, transmission and Fresnel). Dual-source blending allows only one colour output, so the glass pass could not write it itself. Comparing the picture before and after the glass (AMD's own approach, FSR's `GenerateReactiveMask`) was chosen instead: it measures how much of each pixel is the glass's own light (a lit reflection over a dark room counts, clear glass over a bright view hardly does), needs no material code of its own, and would cover particles too.
  - **A share, not a difference:** AMD's helper tone maps the colours first, which needs the exposure; eye adaptation runs after anti-aliasing, so this frame's is not known yet. The change as a share of the brighter picture is the same at any exposure.
  - **Readers:** FSR's reactive input, DLSS's bias towards the current colour (Streamline's `BiasCurrentColorHint`, the mask the DLSS entry above kept in reserve for trails), and the engine's own resolve (`taaReactiveShare`). Limited to 0.9 by default, so glass that makes the whole pixel keeps a little history and its edges stay smoothed (untuned).
  - **Found when the user ran it:** Streamline logged "Cannot have undefined format" every frame. Its Vulkan format table (read in its source, `sl.chi/vulkan.cpp`) maps R8_UINT, R16_SFLOAT, R32_SFLOAT and R32_UINT among single channels, but not R8_UNORM, the mask's first format; the mask became R16_SFLOAT.
  - **Found on the way:** the PowerShell backtick trap in the tooling notes struck: a double-quoted replacement turned the architecture's code spans into control characters (git saw the file as binary). Restored and redone with the editor; every changed file scanned for control characters since.
  - **Checked:** the tests (debug and release, tests first). Not checked here: the picture and the validation layers, left to the user; the GPU test cases are written and built, not run; golden images not run (only views with glass change, and only under anti-aliasing).
- `glass-shadows` (Oct 24 2026): the second part, stacked on `transparency-motion`. The user chose ray-traced tinting.
  - **Where the ray runs:** a compute pass before the main pass (as the mirrored lights do) was weighed against the main pass itself. The pass would have kept the main pass's 16 pipelines, but one image per pixel holds one light, so only the sun would have been tinted; the user wanted spot lights too. So the main pass got copies of its pipelines that trace glass (32 in all, the copies built only on GPUs with ray queries and used only while glass is in the world, so other frames draw exactly as before).
  - **Cheap where there is no glass:** each ray is first tested against the glass's bounding spheres (all of the world's glass, not only what is on screen); most pixel and light pairs pass no glass and trace nothing. Only lights with shadow maps get glass rays: a light without shadows already shines through walls, so tinting it at glass alone would be inconsistent.
  - **One tint per pane:** a pane modelled as a thin box would tint twice if both faces counted. Only faces turned towards the light count, as the camera draws only the faces turned towards it. Each triangle is reported once per ray (`NO_DUPLICATE_ANY_HIT` on every mesh's structure, since a mesh's structure does not know its material).
  - **Fresnel at the beam's angle:** the view's blend uses the specular albedo (the rough lobe's average); a beam crossing a smooth pane meets it at one angle, so the shadow takes Schlick's Fresnel there. The rest of the formula is one shared function with the blend.
  - **Rays too:** probe rays and traced reflections are tinted by the glass in front of what they meet, and their hits' shadow rays by the glass towards each light, so the bounce through the lab's window will take a tinted pane's colour. The mirrored lights' rays are not (left for later, with coloured shadow maps as the fallback without ray queries).
  - **Checked:** the tests (debug and release, tests first). Not checked here: the picture, the cost and the validation layers, left to the user; the GPU test case is written and built, not run; golden images not run (views with glass change on ray-tracing GPUs).
- `mirrored-light-glass` (Oct 24 2026): the glass shadows' leftovers. The user chose to finish the ray-traced path now (the mirrored lights) and move coloured shadow maps, the fallback without ray queries, to phase 14 beside the other fallbacks.
  - **Both legs:** the light's shadow ray to the mirror became `lightThrough` (per channel), and the light leaving the mirror passes `glassTransmittance` towards the receiver, along the path's centre (a rough mirror's footprint samples share it: they cross the same pane a hair apart).
  - **No new maths:** both irradiance formulas are linear in the light's intensity, so the tint rides in with it (a rough mirror's tinted sample mean goes in as the intensity, the mean weight 1); no CPU mirror changed, so no new test. The tests still pass (debug and release) and the four shaders that share the search compile; the picture and validation are left to the user.
- `refraction` (Oct 24 2026): the last of the three transparency parts.
  - **The method:** the screen-space one Unreal and Filament use. The picture before the glass is kept with ever blurrier levels; thick glass bends its view ray (Snell's law, Filament's solid sphere: one bend, then the thickness along the bent ray) and reads the picture where the ray leaves; frosted glass reads it as blurred as its roughness. Its limits, accepted when it was laid out: only what is on screen is seen through it, and glass behind it is missing.
  - **Depth-aware blur:** frosted glass blurs what is just behind it little and what is far behind it a lot, the look that tells frosted glass from a blurred picture. So the picture's level 0 holds each pixel's distance along the view in alpha (a pass of its own reading the depth), and the blur is the cone's width, (ior − 1) alpha over the distance behind (a thin prism's deviation per facet tilt), seen from the camera.
  - **Which glass refracts:** thick glass, and thin glass from roughness 0.05, where the blur can reach a few pixels. The lab's window (0.02) keeps blending, which keeps glass behind glass visible and costs nothing.
  - **No new blend:** refracting glass adds the light it reads to its own and lets only its uncovered share through, so the dual-source blend stays as it was.
  - **One pyramid builder:** the screen-space reflections' colour pyramid became `VulkanColourPyramid`, shared with the picture before the glass, and the reactive mask now compares against that picture's level 0 instead of a copy of its own.
  - **Test pieces:** made in the light lab rather than downloaded (no licences to check): a ball of solid glass (thickness 2 model units, the unit sphere's diameter, scaled with the ball) and a frosted panel in front of the chrome ball, so the ball is seen blurred behind it while the slab just behind stays nearly sharp. The loader reads `KHR_materials_volume` and `KHR_materials_ior` for models that have them.
  - **Found on the way:** the comment above `drawTransparent` described the reflection probe's capture (it had come apart from `drawProbeCapture`); moved back.
  - **Checked:** the tests (debug and release, tests first). Not checked here: the picture and the validation layers, left to the user; the GPU test case is written and built, not run; golden images not run (lab views with the new pieces or the window change).
- `volumetric-fog` (Oct 24 2026): the last item of 6e. The user took all four recommendations after the pros and cons: a 160 x 90 x 64 froxel grid of its own (not the clusters' 64-pixel cells, which would have made beams blocky) reading the clusters' light lists, the probes' light from all around, the height fog's closed form past a 100 m range, and a `Fog` component with a ground mist in the light lab.
  - **The physics:** single scattering. Along a view ray the surface's light is dimmed by exp(-optical depth) and the air adds, at every point, its extinction times its albedo times the light arriving there weighted by the Henyey-Greenstein phase function and the light's shadow. In the engine's units (a light's value its irradiance, a pixel pi times radiance) a light adds pi p E per unit of optical depth; a uniform sky summed over every direction gives back its own radiance whatever g (tested over 8,192 directions).
  - **Three passes:** the grid lit (each froxel's extinction from the height fog and its light: the sun through its cascades with one comparison, the lamps its cluster lists with theirs, the probes' light from above and below), integrated column by column front to back (the slab's exact solution, so one thick slab equals many thin ones), and the picture before the glass dimmed and lit by the grid at each pixel's depth. Glass applies the fog to itself: it adds its own light dimmed by the fog in front and the fog's light only for the share it does not let through, which is exactly the glass and the surface behind fogged in turn (tested).
  - **Smoothness:** each froxel's sample moves round its middle on a 16-frame Halton cycle and keeps 90% of last frame's grid, read where its point was then (the grid follows the camera without the jitter, so it works with anti-aliasing off too).
  - **Slices:** even in log(depth + 1 m), so a few centimetres deep at the camera and about 7 m at 100 m, as a froxel's width grows.
  - **Past the grid:** the height fog's optical depth has a closed form along a ray, (1 - exp(-x)) / x times the start's density and the length, with its series near 0 for level rays; lit by the directional lights and the flat ambient without shadows, out to the fog's farthest distance (1 km by default) so the sky beyond still shows.
  - **Not done as planned: waking the probes in the air.** `docs/ILLUMINATION.md` noted that fog would need the probes in empty air awake. Waking every sleeping probe in the cascades would trace up to the whole 22,932 against today's 3,700 to 5,600 and pass the 8,192 traced a frame, so every surface's probes would take turns. Instead the fog reads only traced probes, as surfaces do, falling back to the coarser cascades (whose larger cells nearly always reach the ground, so their probes are traced) and then the flat ambient. Air metres above the ground thus takes the coarser light; if that shows, a wake-up limited to the densest cascades near the camera is the next step.
  - **Known limits:** lights without shadow maps light the fog through walls; past the sun's last cascade (the farthest surface on screen) the sky's fog has no sun shadows; refracting glass reads the fogged picture and adds the front fog's light for its covered share, counting that part twice (small); the fog dims no light on its way to a froxel; the reflection probe's capture and the probes' rays see no fog.
  - **On the way:** `VulkanImage` gained 3D images (`ImageDescription::volume`); the shadow tiles' declarations moved from the main pass into `scene_shadow_tiles.glsl`, shared with the fog's grid; the clusters' lists' barriers now cover the fog's compute reads; `probeAirWeight` joined the probes' sampling.
  - **Checked:** the tests (debug and release, tests first: the medium and its closed form against a fine sum, the slices, the grid's coordinates both ways through a real camera, the jitter, the phase function's furnace, the slab and its energy conservation, glass over the fog, the column, the record, the presets, the light lab's mist). Everything builds without warnings, the shaders compile. Not checked here: the picture, its cost and the validation layers, left to the user; the GPU test cases are written and built, not run; golden images not run (the light lab's mist is on by default, so every lab view changes).

## Phase 7: Cloth simulation (Oct 24 2026 onwards)

- `job-system` (Oct 24 2026): the engine's first threads, so the cloth solver (and later frame building, culling and asset decoding) can use every core.
  - **Our own, not a library:** enkiTS and Taskflow were the alternatives. The user chose our own: about 300 lines that fit the engine's determinism rules, no new dependency.
  - **The design:** work stealing, a pile per thread, `parallelFor` halving its range down to a grain. The waiting thread runs jobs, so a `parallelFor` inside a job finishes.
  - **Locked piles, not lock-free:** a deque behind a mutex per thread. The lock-free Chase-Lev deque is much harder to get right, and with pieces of microseconds the lock costs little. Worth revisiting only if a measurement shows the locks.
  - **Determinism:** each index writes only its own output; sums go by fixed chunks in a fixed order. Tested: the same bits on 1 and 8 threads.
  - **First user, the BRDF table** (32 x 32 texels, 4096 samples each): one texel a job. Measured in release, best of five: 103.6 ms on one thread, 10.5 ms on 16 (this PC's threads), the table the same to the last bit.
  - **The setting:** `EngineSettings::workerThreads` (-1, one per core but one; start-up only), so a slow or misbehaving run can be tried on one thread.
  - **Checked:** the tests (debug and release, tests first: every index once for empty, tiny, uneven and large ranges, 1 thread against 8, the chunked sum, nesting, exceptions with and without workers, a grain of 0, work reaching several threads, two outside threads at once). The game and GPU test program build without warnings; nothing run on the GPU.

- `cloth-lab` (Oct 24 2026): a third game, `--game clothlab`, asked for by the user as the place to build and judge cloth, as the light lab is for lighting.
  - **What it holds:** a station for each of phase 7's milestones, kept apart from the game as a layout the tests check: a ball on a pedestal under a pin bar (a sheet pinned at two corners drapes over it), a mat (a sheet dropped onto itself piles there), a wide bar (silk beside denim).
  - **Kept plain:** a pale floor and matte surfaces so the cloth stands out, a fixed afternoon sun from the side so folds cast shadows, the real sky and light probes; none of the light lab's clock, night or street.
  - **Not yet:** a capsule (phase 5's other shape for body collision) waits for the collision item, since the engine has no capsule mesh; the mannequin's distance field comes later in the phase.
  - **On the way:** `enclosing(a, b)` in `math/aabb`, the box holding two boxes, shared with `sweepSphere`, which had its own copy.
  - **Checked:** the tests (debug and release, tests first: registered, the ball on its pedestal under its bar, the frames standing on the floor, the stations apart and clear of the start, the level matching the layout, the sun, sky and probes, golden views). The game builds without warnings. Not checked here: the picture, left to the user; its golden reference is not saved yet, so `make golden` reports it missing until `make golden-save GAME=clothlab`.

- `dynamic-meshes` (Oct 24 2026): deforming meshes, whose vertices move every tick (cloth), drawn by the engine's renderer. Until now a mesh was uploaded once and never changed.
  - **Explained first, then chosen by the user:** normals on the CPU (simple, testable, the solver starts on the CPU) rather than a compute pass; ray tracing now (cloth in GI, reflections and ray-traced shadows from the start) rather than leaving it out; a rippling banner in the cloth lab to see it before cloth exists.
  - **Normals and tangents:** area-weighted sums of the triangles' cross products, smooth across UV seams; tangents by Lengyel's method from the UVs, made square to the normal, keeping MikkTSpace's side from the rest shape. The rest shape's vertices are worked out the same way, so nothing changes when a sheet stops moving.
  - **Blended between ticks** like transforms (`previousPositions`, saved by `runFixedUpdate`), and hashed into the simulation's state.
  - **The ring of three buffers:** one more than the frames in flight, written by the CPU directly; a shape that holds still (the same fingerprint) is not uploaded again and has no motion. Tested over many orders of new and held frames that no slot a frame in flight reads is ever written.
  - **Shadows:** `DrawItem::shape`, a hash of the frame's positions, joins the shadow cache's fingerprint, so a sheet that moves redraws its tiles and a still one keeps them.
  - **Motion vectors:** the velocity pass reads last frame's slot as a second vertex stream; other meshes bind their one buffer twice, so their motion is as before.
  - **Ray tracing:** a structure per slot, rebuilt (fast build, no compaction) when its slot holds a newer shape, in the frame before the top-level build.
  - **Measured** (release, best of 50): recomputing the banner's 841 vertices 0.045 ms, a 100 x 100 sheet 0.50 ms (more than the 0.2 ms guessed). One mesh is one job; splitting a big sheet across threads is left until a solver needs it.
  - **On the way:** `Mesh::grid`; `VertexLayout` takes a second vertex binding.
  - **Checked:** the tests (debug and release, tests first: the grid, the topology and its seams, normals and tangents of flat, turned, bumped and squashed shapes, blending, ticks saving shapes, the fingerprint, attaching shapes and the shadow cache, the asset manager, the ring, the banner pinned, moving and deterministic). The game, release game and GPU test program build without warnings and the shaders compile. Not checked here: the picture, the validation layers and the GPU cost, left to the user; golden images not run (the cloth lab has none yet).

- `cloth-particles` (Oct 24 2026): the cloth's points, the first piece of the cloth module (`src/cloth/`, a new folder above `physics/` and `scene/`, as the roadmap asked for cloth to be a module of its own).
  - **Kept small, the user's choice:** the store, its masses, pins and step alone, without the XPBD solver, so each idea is explained and tested by itself. Nothing new shows yet: without constraints a sheet's points just fall.
  - **Structure of arrays:** positions, previous positions, velocities and inverse masses as separate arrays, so a loop reads only what it needs and the compiler can work on several floats at once.
  - **Inverse masses:** XPBD moves points in proportion to them, so a pinned point is simply infinitely heavy.
  - **Lumped masses from the fabric's mass per area** (0.2 kg/m2 by default), a third of each triangle to each corner, so fabrics differ by a real number.
  - **Symplectic Euler, not phase 5's `fly`,** though the roadmap suggested reusing it: the step works out each velocity from the motion afterwards, which gives back v + a t / 2 from `fly`'s v t + a t^2 / 2, so a sheet would fall at half of g. The roadmap's note now says `bounceVelocity` fits the collision item instead.
  - **Checked:** the tests (debug and release, tests first: the masses add up to the fabric's and split as the triangles do, bad meshes and densities rejected, free fall at g, velocity from the motion, pins hold, a cloth entity steps and feeds its mesh, its state in the hash, the defaults). The game builds without warnings.

- `xpbd-cloth` (Oct 24 2026): the cloth solver, XPBD (Macklin, Muller & Chentanez 2016) in substeps (Macklin et al. 2019), with stretch constraints along every edge and fixed pins; the first real cloth, a sheet in the cloth lab in place of the banner.
  - **Explained first, then chosen by the user:** linear drag for the air (a sheet settles; true air resistance by each triangle's facing could come later), 15 substeps a tick, and the banner replaced by the real sheet.
  - **One pass a substep,** so the multiplier starts at 0 each time and needs no storage: delta lambda = -C / (w1 + w2 + alpha / h^2).
  - **Compliance from real cotton:** 1 / (E t), about 2000 N/m, resolution-independent on a square mesh; a hanging sheet stretches about 0.15%.
  - **Changed on the way:** the sheet starts flat, not hanging. Pinned at its two top corners a sheet-width apart, a hanging sheet's top edge is already straight at rest length, so it could not move at all.
  - **Float precision found by the Hooke test:** a weight on a compliant edge settles 0.4-0.5% off m g alpha, because each substep's gravity move (micrometres) is rounded to the float grid at a metre from the origin. Tested to 1%; invisible for cloth; points relative to the cloth's own origin would cure it for large worlds.
  - **Measured** (release, one thread): the lab's 28 x 30 sheet 0.37 ms a tick (about 4% of a core), a 100 x 100 sheet 4.2 ms (as estimated, too slow on one thread).
  - **Roadmap:** stretch and fixed pins are done with the solver; the constraints item keeps bending and moving pins.
  - **Checked:** the tests (debug and release, tests first: the edges of a grid, a stiff edge corrected with its centre of mass kept, pinned ends, Hooke's law at 5, 15 and 30 substeps, a pendulum keeping its length and slowed by drag, a pinned sheet hanging barely stretched and the same twice, the lab's sheet pinned, hanging in its station and fed to its mesh). The game builds without warnings. Not checked here: how it looks, left to the user.

- `cloth-grab` (Oct 25 2026): dragging the cloth lab's sheet with the left mouse button, asked for by the user after seeing the first cloth.
  - **Explained first, then chosen by the user:** a stiff spring (compliance 0.001 m/N) rather than an exact hand, one point rather than a pinch of three, the left button.
  - **The hand is one more XPBD constraint,** solved after the stretch constraints each substep; the game sets it from a command made once a frame in `onInput`, so input never reaches a tick.
  - **Pointing:** `rayThroughPixel` moved from the demo into `PlayerControls`, beside a new `pointerRay` (the crosshair while the mouse looks, the cursor in MOBA mode), so both games share one.
  - **Corrected on the way, by the tests:** "let go, the point keeps its speed" was the wrong claim for a sheet pinned at the top, since the edges the hand stretched snap it back within a tick. The test now tows a free sheet through empty space: its centre of mass keeps the velocity it had when released (momentum conserved, 0.1% float drift over half a second). Towed with no drag, the sheet sloshes on the hand's spring (its centre of mass between 0.8 and 1.9 m/s at a 1.2 m/s tow).
  - **Not grabbing on the capturing click:** in first person and the free camera, the click that recaptures a freed mouse grabs nothing.
  - **Checked:** the tests (debug and release, tests first: rays find the sheet as it is now and the nearest corner, reach and direction, the hand exact and as a spring, pinned points, a held point following, momentum after release, the pointer ray in each mode, the lab's drag through real input: grab, lift, release, the capturing click, out of reach). The game builds without warnings. Not checked here: how it feels, left to the user.

- `cloth-bending` (Oct 25 2026): bending (a hinge-angle constraint on every inside edge) and moving pins; the lab's sheet now hangs from two hooks that slide along the bar.
  - **Explained first, then chosen by the user:** the dihedral angle (physical units, curved rest shapes for garments) over isometric bending or springs across hinges, and a Hook gap slider.
  - **Stiffness in real units:** the bending rigidity B (N m) the cantilever test measures, a hinge's compliance 1 / (B 3 l^2 / A) from discrete shells; the default, 10 micro N m, gives cotton's 1.7 cm bending length.
  - **The gradient's sign, found by the tests:** Bridson's closed form matched the finite differences in size but was exactly opposite in sign to the first angle convention; the angle is now measured the other way, so the published formula stands as written.
  - **Convergence limit, found by the tests:** the same B should droop alike on two meshes, but at 15 substeps a mesh twice as fine droops 75% more for a very stiff strip (60 substeps: 20%), since one Gauss-Seidel pass a substep spreads stiffness about a hinge a pass. Real fabrics agree within 4%. The test checks the gap shrinks with the substeps; the roadmap's parallel solver item notes it.
  - **Pins move smoothly,** slid through the substeps, and drag no longer slows them (their velocity is their motion, which the game drives).
  - **Cost, measured:** 2.0 ms a tick for the lab's sheet, against the 1 ms estimated (2.8 ms at first: the angle and the gradient each built the hinge's frame, now shared, and the normals are no longer normalised for atan2). atan2 takes only 4.5 ns and wrapping the angle made no difference (that change was reverted); the rest is the hinge's arithmetic, about 45 ns. The parallel or GPU solver is where the rest must come from.
  - **Checked:** the tests (debug and release, tests first: the hinges of a sheet, angles both ways, the gradient against finite differences and summing to zero, a folded hinge opening, the cantilever stiffer drooping less and the resolution gap shrinking, moving pins' path and velocity, a strip carried by its pins, the same run twice, the lab's hooks sliding and stopping at the sheet's width). The game builds without warnings. Not checked here: the folds' look, left to the user.

- `cloth-collision` (Oct 25 2026): cloth against bodies: every `Collider`'s sphere, capsule or box, with friction; the first milestone, a sheet pinned at two corners draped over the cloth lab's ball.
  - **Explained first, then chosen by the user:** friction on the positions (holds a resting sheet exactly still, the XPBD standard) rather than phase 5's `bounceVelocity` after the step, and a second sheet at the drape station rather than moving the hanging one.
  - **Contacts are one-sided constraints with no give,** solved last each substep so nothing is left inside a body, keeping a 5 mm thickness so cloth rests on a body rather than flickering half inside it.
  - **Layout corrected, found by a probe:** the cloth lab's ball stood 0.75 m in front of the drape bar, so the sheet fell, tapped it once and swung through to hang straight down behind it, a pendulum. Moved back to 0.2 m in front of the bar, the hanging sheet cannot hang straight and rests draped over the ball (in contact at 0.5 s, still by 2 s).
  - **Measured:** collisions cost 0.36 ms a tick for both sheets (release); the two sheets 4.2 ms a tick in release, half a core, and 31 ms in debug, where the lab now runs at about a quarter speed (one sheet with bending was already near half speed in debug). The cloth wants the release build until the solver uses more cores; the roadmap's parallel solver item says so.
  - **Roadmap:** the collision item is done for phase 5's shapes; the mannequin's distance field and moving bodies are an item of their own.
  - **Checked:** the tests (debug and release, tests first: signed distances and normals for every shape, inside and out, points pushed out to the thickness, clear and pinned points untouched, friction sticking, slipping and off, a point resting on a ball below its friction angle and sliding above it, a sheet dropped on a ball resting on it with nothing inside, the same twice, the world's colliders placed by their entities, the defaults, the lab's colliders matching its layout and the drape sheet settling over the ball). The game builds without warnings. Not checked here: how it looks, left to the user.

- `cloth-parallel` (Oct 25 2026): the cloth solver on many threads: a job per cloth, coloured constraints solved on every core, spinning workers, the cloth code optimised in the debug build, and a big sheet in the cloth lab to measure it.
  - **Explained first; the user chose all three with a big sheet,** against my recommendation to leave colouring until garments needed it.
  - **Bit for bit:** the colouring is also the solving order on one thread, so one thread and many give the same result (tested). The order changed from one sweep to colour by colour, so one test's number moved: a dragged point jiggles a few millimetres at about 80 Hz on the hand's spring, so the grab test now checks its average speed, not one tick's.
  - **Debug at real time:** `src/cloth/` compiled at `-O2` in the debug build: the lab's two sheets 31 ms a tick before, 3.3 ms after.
  - **Measured, and tuned by sweeps** (release, the 100 x 100 sheet): 26 ms a tick on one thread, 9.3 ms on 16 (2.8 times); the lab's two sheets 4.0 to 2.36 ms.
    - Job sizes: smaller jobs cost more to hand out, larger left threads idle; stretch 2048 and bending 256 measured best.
    - Spinning: workers that spin 50 microseconds before sleeping, 9.3 ms; sleeping at once, 13.8. A pause instruction instead of yielding made no difference.
    - Contacts shared among threads measured slower every time and stay on the caller.
    - 16 threads were often worse than 4 (stretch alone 8 ms against 1.2 in one sweep): this CPU's 16 threads are 8 cores, and a spinning worker slows the thread working beside it on the same core.
  - **The limit:** the barrier after each colour, about 25 microseconds, 210 a tick, against about 6 of work per colour. A faster barrier (fixed-thread loops meeting at a spinning counter, one thread per physical core) or the GPU is a roadmap item.
  - **The big sheet is off by default:** at about 9 ms a tick it is more than a tick's 8.3, so with it on the lab runs a little slower than real time. On from F5's Stations (read at start-up) to measure.
  - **Games get the job system** (`Game::getJobs`), `serialJobs()` until the engine gives its own, so games run headless on one thread unchanged.
  - **Seen on the way:** the debug tests take about 80 s; the slowest are the golden views, cloth lab and demo tests (about 14 s each), DDGI (11 s) and the light lab (8 s). Not changed here.
  - **Checked:** the tests (debug and release, tests first: colours sharing no point and covering every constraint, a busy sheet with pins, a hand and a ball bit for bit the same on 1 and 8 threads, three cloths stepped together as one by one, the lab's big sheet sagging on its hooks over the mat and none when off). The game builds without warnings.

