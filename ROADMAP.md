# Roadmap

A from-scratch C++20 engine that can host both an **FPS** and a **MOBA**, and eventually a **dressmaking game** with real cloth physics (like Dressmaker, but more realistic), and a **solar system** simulated at real scale. The core stays genre-agnostic; genre code (camera styles, weapons, abilities, netcode tuning, cloth and sewing, orbital mechanics) lives in modules on top.

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
| 9. Rigid-body physics: orientation, angular velocity, mass and inertia, collision | Collision in phase 5; full rigid-body dynamics in phase 13 |
| 10. Engine architecture: scene management, entities and components, resource loading | Phase 3 |
| 10. Engine architecture: debug tools | Debug drawing in phase 4; profiler in phase 5; debug UI in phase 8 |

## Phase 3: Engine architecture (done, Oct 8 2026)

Untangle `Application` and the old `GpuDisplay` before adding more features, so everything after this has a clear place to go.

- [x] Entity/component layer (`entity-component`): entities with generations, sparse-set component storage, `World::each` systems; `Transform`, `PreviousTransform`, `ModelRenderer`, `Spinner` and `CharacterMovement` components replace `Scene`, `MeshInstance` and `ModelInstance`
- [x] Split `GpuDisplay` (`platform-split`) into `Window`, `GpuRenderer` and a richer `Input` (mouse buttons with exact click position and timestamp, cursor, window size); ground clicks, steering and edge panning moved into game code
- [x] `Game` interface (`game-interface`): `onInit`, `onInput`, `onFixedUpdate`, `onUpdate`; `Application` is the engine loop and the demo is `DemoGame`
- [x] Camera split (`camera-split`): `Camera` is a lens (position, orientation, FOV, aspect) with matrices built on request; free-fly, first-person and MOBA are separate concrete controllers, with look angles stored as yaw/pitch
- [x] Asset manager with handles (`asset-manager`): `MeshHandle` and `TextureHandle`, each file loaded once, GPU copies indexed like the handles, no per-frame path lookups
- [x] Indexed meshes (`indexed-meshes`): `Mesh` stays the import format; `buildIndexedMesh` produces the render format (`MeshVertex` position/normal/UV, the exact GPU layout, plus 32-bit indices), merging identical corners

**Milestone:** the current demo runs as a `Game`, and a test runs its simulation for 1000 ticks with no window. ✔ (`tests/demo_game_test.cpp`; the remaining items finish the phase)

## Phase 4: Rendering upgrades (done, Oct 8-9 2026)

Finishes the old "shading and lighting" step and adds the tools the next phases need to see what they are doing.

- [x] Light data in a uniform buffer instead of hardcoded in the shader; several directional and point lights (`lights-uniform-buffer`)
- [x] Specular highlights (Blinn-Phong) (`specular-highlights`)
- [x] Spotlights (`spotlights`): cone with inner and outer angles, up to 4; the demo gets a night scene and an F-toggled flashlight (at the camera, or carried by the duck in MOBA mode)
- [x] Emissive materials (`moonlight`): surfaces that glow with their own light; the demo gets a faint moonlight and a glowing moon sphere you can fly to in free camera mode, and its MOBA marker glows instead of using the temporary unlit flag
- [x] Optional smooth normals for models that have none (`smooth-normals`): opt-in per load, angle-weighted, grouped by position, with a crease angle
- [x] Normal matrix computed once per object on the CPU; move the depth range correction into the renderer (`cpu-normal-matrix`)
- [x] Linear filtering and mipmaps (`texture-filtering`): trilinear plus 16x anisotropic, mip chains generated on the GPU at upload
- [x] Gamma-correct colour (`srgb-colour`): image files store sRGB (gamma-encoded) values, but textures are created as `UNORM`, so lighting, blending and mipmap averaging treat them as linear light and come out slightly too dark. Use `R8G8B8A8_UNORM_SRGB` textures and an sRGB swapchain so all maths happens in linear light; data that is not colour (future normal maps) stays `UNORM`. Material and light colours will need retuning, since the whole scene will look brighter
- [x] Shadow mapping
  - [x] Shadows from every light (`shadow-mapping`): one depth atlas; point lights take six tiles (cube faces), spotlights one perspective view, directional lights one texel-snapped orthographic box around the camera; normal offset plus slope bias, 3x3 PCF; `castsShadows` on lights and models (default on). The demo's moon was a shadow-casting point light inside the moon sphere for a while, then went back to a directional light (sharper shadows)
  - [x] Tiles sized by the light's reach (`light-priority`): 256, 512 or 1024 pixels for about 0.04 units per texel, the least important shrinking first when the atlas is full, packed as a quadtree; room for 16 shadowed point lights and 8 spotlights
  - [x] First give the renderer the whole frame (`frame-description`): the engine builds a frame description (every `{mesh, matrix, material}` to draw, plus lights and camera) and calls one `render(frame)`, instead of calling `drawMesh` per object. Shadows need it (the scene is drawn twice), frustum culling needs it, and it is the basis for a later ray-tracing backend
- [x] Debug drawing (`debug-drawing`): lines, boxes, spheres and capsules drawn on top of the scene; games add shapes in `onDebugDraw`, and the engine draws every mesh's bounding box when the game asks (demo: F4, with light markers and the walk target). Colliders join the view once phase 5 adds them
- [x] Dithering (`dithering`): the main shader adds up to half an 8-bit step of per-pixel noise (interleaved gradient noise) in sRGB before output, so slow dark fades (the flashlight's pool at night) no longer show rings; mirrored and tested in `core/dither`
- [x] Frustum culling (`frustum-culling`): six planes from any view-projection (`math/frustum`), a bounding sphere per draw from its mesh's bounds, the camera's view culls the main pass and each shadow tile culls with its own light view (never the camera's, so off-screen things still cast shadows); `RenderStats` counts drawn and skipped meshes
- [x] Light priority (`light-priority`): seats in the shader and shadows go to the lights that matter most instead of the first ones in component storage (which shuffles when components are removed). Lights whose range sphere is outside the view are skipped; the rest are ranked by brightness at a focus point the game names (the camera, or the MOBA camera's ground point), `intensity / (d^2 + r^2)` without the range window so far lights do not tie at zero; last frame's seats and shadows count 1.25 times as much (hysteresis), so similar lights do not swap every frame. Seats raised to 64 point and 8 spotlights. The demo gets a field of 42 lamps with pillars (L) and a light count line in the F4 readout
- [x] Light and shadow follow-ups, in this order (each is tested in the game before the next starts)
  - [x] 1. Manual priority (`light-manual-priority`)
    - `int priority = 0` on `PointLight` and `SpotLight`. A higher priority always wins a seat and a shadow over a lower one; importance and hysteresis decide only between equals
    - A game marks a boss's aura or a story lamp, and it is never dropped while in view
    - Changes only the ranking function, plus tests
  - [x] 2. Fading handovers (`light-fading`)
    - Each seated light has a strength between 0 and 1 for its light and another for its shadow. Strengths ramp over 0.25 s, and the shader scales by them
    - A light losing its seat or shadow keeps it while fading out, so the caps count fading lights too; the winner fades in afterwards
    - Lights just switched on or come into view appear at full strength (nothing showed them before); lights leaving the view go at once
    - Needs: `frameSeconds` in `FrameDescription`, strengths in `LightHistory`, and a shadow strength per slot in the shader (light strength is multiplied into the radiance on the CPU)
    - Comes before caching because it changes how slots are handed over
  - [x] 3. Give back tiles for cube faces that see nothing (`shadow-face-culling`)
    - A face whose view pyramid does not overlap the camera's view cannot shadow a visible pixel, so it gets no tile and no draw
    - `Frustum::mayIntersect`: the corners of each view against the other's sides, both ways round (conservative). A left-out face keeps its place in the tile list with an empty rectangle, so the shader's `firstTile + face` lookup is unchanged, and it counts as lit
    - In the demo's lamp field it mostly skips the face that points back towards the camera (about 16 of 96 faces); the F4 readout counts tiles drawn and skipped
    - Frees atlas room and draw time for caching to build on
  - [x] 4. Shadow caching (`shadow-caching`)
    - Tiles stay where they are between frames and are redrawn only when the light moves, the tile changes, or something that casts shadows moves inside its view
    - `ShadowAtlasLayout` (`render/gpu/shadow_atlas`): each tile keeps its square while its light (by entity) and face ask for the same size; a buddy allocator merges freed quarters back; if scattered free space cannot fit a new tile, everything is repacked once
    - `ShadowTileMemory` (`render/gpu/shadow_cache`): what was last drawn into each square (matrix plus a hash of the casters' meshes and model matrices); drawing a square forgets anything it overlaps
    - The atlas is loaded instead of cleared; a redrawn tile is first reset by a "draw depth 1" triangle (`shadow_clear.vert`); with nothing to redraw there is no shadow pass
    - In the demo's lamp field, standing still, about 49 of 82 tiles are cached; the rest see the spinning objects, the flashlight or the moon box. The F4 readout counts tiles drawn, cached and skipped
    - [x] Shadow cap raised from 16 to 64 point lights (`more-point-shadows`), as many as the shader's seats: the atlas still fits everything at the smallest tile size. `LightLimits` lets a caller cap shadows below the seats (the tests do, to keep shadow-only handovers covered). The shader now skips a light, and its 9 shadow lookups, for pixels beyond its range. All lamps in view in the demo cast shadows
  - [x] 5. Soft shadows (PCSS, `soft-shadows`)
    - The shader first searches the shadow map near the point to estimate how far away the blocker is, then widens its blur by `emitterRadius * (receiver - blocker) / blocker`: the penumbra geometry of a solar eclipse
    - A new `emitterRadius` on point lights (0.05) and spotlights (0.025; first 0.1 and 0.05, which looked too soft) instead of `sourceRadius`, which was tuned as a falloff softener far bigger than any real bulb (4.5 on the demo flashlight)
    - The atlas is bound a second time with a plain sampler for the blocker search; 16 + 16 Poisson-disc samples, rotated per pixel; receiver plane depth bias so wide filters do not shadow sloped surfaces; filter 1 to 12 texels, kept inside the tile
    - Directional lights keep the 3x3 filter: a real sun or moon's penumbra is smaller than a texel of the 30-unit box
- [x] Faster builds (`faster-builds`): a one-line change took about 76 s (27 s for the game, 49 s for the tests); now about 2 s, a clean build of everything about 10 s
  - [x] Compile each `.cpp` to its own object file in `build/obj/`, and only link at the end, so a change recompiles only what it touches
  - [x] Let GCC record which headers each file uses (`-MMD -MP`), so a header change recompiles exactly the files that include it
  - [x] Build in parallel on every core (`-j`), with output kept in order per file
  - [x] The game and the tests share the engine's object files, so the engine compiles once instead of twice
- [x] Low-latency presentation (`platform-split`): one frame in flight, configurable present mode, mailbox by default

**Milestone:** a lit scene with shadows, and a toggle that draws every collider and bounding box. ✔ (bounding boxes; colliders join the F4 view in phase 5)

## Phase 5: Collision and movement ← current

What both genres actually need from physics: knowing what you hit and moving a character through a level without passing through walls.

- [x] Settings in one place (`settings-structs`), done before physics adds dozens of new constants: tuning values live in one settings struct per system, owned where the system lives, read every frame and editable through `getSettings()`: `EngineSettings` (tick rate, frame time clamp, frame cap, present mode, and `RenderSettings`: `LightPrioritySettings` for seats, shadow caps, hysteresis and fade time, `ShadowSettings` for texel target, directional distance and reach, normal offset), and a settings struct per camera controller (sharing `LookSettings` for mouse sensitivity and sprint). Merged on the way: the first-person and free cameras' shared look-and-move steps (`turnCamera`, `moveCamera`), and angle wrapping (`math/angles`). Numbers the shaders share with C++ (light array sizes, soft-shadow filter limits, the dither step) now come from macro-only headers both include (glslc gets `-Isrc` and dependency files). Physics settings (gravity, substeps, cloth stiffness) start out in the same shape
- [ ] Shapes: sphere, capsule, axis-aligned box; overlap tests between them
- [ ] Raycasts against shapes and against triangle meshes
- [ ] Mouse picking: click a unit or object, not only the ground
- [ ] Broad phase: uniform grid, so tests only check nearby objects
- [ ] Static level collision against triangle meshes
- [ ] Kinematic character controller: capsule, sliding along walls, stepping up stairs, ground snapping, gravity and jumping (first person has a camera-only placeholder jump until then, `fps-jump`)
- [ ] Particle physics: forces, gravity, semi-implicit Euler integration, bouncing off level geometry (projectiles, grenades). Keep the particle store plain (positions, previous positions, inverse masses) because it will serve three users: grenades here, phase 7's cloth vertices and phase 12's orbital bodies (which need double precision, so the store's vector type should be a parameter)
- [ ] Frame and system timing (the profiler, moved here from the UI phase): frame time and tick time in the F4 readout, and a simple profiler that times each system per tick. Physics is the first work whose cost grows with the scene, so the switch to the release build below, and every later performance decision, should rest on measurements
- [ ] `release` build target (`-O2`), with small vec/mat operators moved into headers so they can inline (moved here from the optional list). Rendering never needed it, but physics runs many tiny vector operations per tick, and unoptimised code that calls a function for every `+` will be too slow long before the frame rate drops. `make` stays the `-O0 -g` debug build

**Milestone:** walk around a small level in first person with walls, stairs and jumping, and throw a grenade that bounces.

## Phase 6: Lighting: HDR and global illumination

Light that bounces: the biggest visual step up from today's direct-only lighting, and done early because it is one of the two things we most want to see. It needs nothing from phase 5 except the profiler and release build to measure it, so it could even start alongside it. Everything here also serves later phases: the HDR target settles where the HUD goes, compute pipelines run the cloth solver, and physically based materials carry fabric shading.

- [ ] HDR off-screen render target and tone mapping: render the scene to a high-precision off-screen texture, then a final pass rolls brightness above 1 off smoothly instead of clipping to white and writes the 8-bit screen image. The dither moves from `triangle.frag` into that final pass (the off-screen image needs none). It comes before the HUD (phase 8) because it changes where everything drawn on top of the scene goes, so the HUD is built once, over the finished image. Bloom and DDGI build on it
  - Once tone mapping exists, consider physical light units plus a camera exposure for all lights. Emissive surfaces and the lights they stand for could then be linked physically (the light a surface receives from the moon is the moon's brightness times the solid angle it covers), with the exposure step mapping the real dynamic range (the moon disc is 10,000+ times brighter than moonlit ground) to the screen. Until then, the demo keeps the moon sphere's glow and the moonlight as separate artistic dials, because the renderer clips at 1.0
- [ ] Bloom: bright and emissive pixels blurred and added back before tone mapping, giving the moon and lamps a soft halo
- [ ] SDL_GPU compute pipelines: the engine's first compute shaders (`SDL_CreateGPUComputePipeline`, compiled by `glslc` like the others), reading and writing storage buffers and textures. One foundation shared by DDGI's probe updates below, the GPU cloth solver (phase 7) and, if needed, Barnes-Hut gravity (phase 12)
- [ ] Physically based materials (glTF metallic-roughness): what glTF models are authored for. Bounce light depends on how much light each surface reflects, so materials should be physically sensible before global illumination is tuned; fabric shading (phase 9) and the hardware ray-tracing backend (phase 13) build on them too
- [ ] Dynamic global illumination with a probe grid (DDGI, "Dynamic Diffuse Global Illumination", Majercik et al. 2019). Today light stops at the first surface it hits, so a room lit through one window is black wherever the sunbeam does not land, and the ambient light is a flat stand-in for all the light bouncing around. DDGI measures that bounced light at points in space each frame and lets every surface look it up
  - Needs first: the HDR target and tone mapping above (bounce light routinely adds up to more than 1 and would clip to white), and the compute pipelines above
  - [ ] Probe grid: a 3D grid of light probes over the level, one every 1-2 m. Each stores the light arriving from every direction in a small octahedral texture (the sphere of directions unfolded onto a square, e.g. 8x8 texels), and all probes are packed into one atlas, like the shadow tiles. Diffuse light changes slowly across space, so a coarse grid captures it
  - [ ] Probe update in a compute shader: each probe traces a few hundred rays per frame, in directions given a new random rotation every frame so the samples add up to the whole sphere over time. Each hit gets direct light from the existing lights and shadow atlas, plus indirect light read from last frame's probes, so each frame adds one more bounce: infinite bounces for the price of one. Caveat: the shadow atlas only holds lights and cube faces the camera can see (light priority and face culling), so hits outside the view need lights kept for the probes, or go unshadowed
  - [ ] Hysteresis blending: each texel keeps about 97% of its old value and takes 3% of the new one, which averages a few hundred noisy rays into a smooth result over time, like a long-exposure photograph. When a texel's new value differs a lot (a light switched on, a door opened), it blends faster so the change does not lag for seconds
  - [ ] Leak prevention: each probe also stores the mean and mean-squared hit distance per direction (a second small octahedral texture). When shading, a Chebyshev test on these (as in variance shadow maps) estimates whether the probe can actually see the point; a probe behind a wall gets almost no weight, so sunlight in one room does not bleed through into the next
  - [ ] Probe relocation and deactivation: a grid ignores the level's shape, so some probes land inside geometry, where every ray hits a back face and they report darkness. Move each one a short way out into open space, and switch off probes buried inside solid walls (they only waste rays and darken their neighbours)
  - [ ] Sampling in `triangle.frag`: each pixel blends the 8 probes at the corners of the grid cell around it, weighted by distance (trilinear), visibility (the Chebyshev test) and how far each probe lies in front of the surface along its normal, then adds the result as diffuse light in place of the flat ambient
  - [ ] Ray tracing without RTX: rays are traced against a simplified copy of the scene, signed distance fields or a coarse voxel grid of the level, built in compute shaders, because SDL_GPU has no ray tracing. Probes only need blurry diffuse light, so coarse geometry is enough. Phase 13's hardware ray-tracing backend can replace this later
  - [ ] Cascades for large worlds: a dense grid near the camera and coarser ones further out (each about twice the spacing of the last), scrolling with the camera so only the newly uncovered slice of probes starts from scratch. A single dense grid over a whole MOBA map or open level would cost too much memory and too many rays
  - [ ] Debug view: draw each probe as a small sphere shaded with its stored light (and switched-off probes in another colour), so leaks, dark probes stuck in walls and slow updates are visible at a glance

**Milestone:** a room lit only through a window by the moving sun fills with soft bounce light that changes as the sun moves, a red wall tints the floor beside it, and no light leaks through the walls into the next room; the moon and lamps glow with bloom.

## Phase 7: Cloth simulation

The physics half of the dressmaking game, early because it is the other thing we most want to see: fabric that drapes, folds and piles up without passing through itself. Builds on phase 5's particle store and collision shapes and phase 6's compute pipelines. Cloth code lives in its own module on top of the core, like the FPS and MOBA code. Garments, sewing and the game itself follow in phase 9, once phase 8 has skinned characters to dress.

- [ ] Dynamic meshes: a GPU upload path for meshes whose vertices change every frame, with normals recomputed each time (today a mesh is uploaded once and never changes)
- [ ] XPBD solver (extended position-based dynamics): particles integrated with gravity, then constraints project their positions back towards valid states. Each constraint has a compliance (inverse stiffness), so nearly unstretchable fabric stays stable at the 120 Hz tick, where stiff springs would need a tiny timestep. Use many substeps (10-20 per tick, one constraint iteration each) rather than many iterations of one big step: small steps converge faster for the same work, so fabric stretches less (Macklin et al. 2019, "Small Steps in Physics Simulation")
- [ ] Cloth constraints: stretch (edge lengths), bending (angle between neighbouring triangles), pinning (fixed points, for testing and for hangers)
- [ ] Collision with bodies: spheres and capsules first (phase 5's shapes), then a signed distance field around a mannequin; friction between cloth and body
- [ ] Self-collision, so folds and layers never pass through each other. A broad phase (a BVH or spatial hash) only finds nearby pairs; cloth is so thin that a fast-moving layer can jump right through another between two substeps, so it also needs continuous collision detection (vertex-triangle and edge-edge pairs checked along their motion during the step) and a cloth thickness that keeps layers apart
- [ ] Untangling pass (Baraff, Witkin & Kass 2003, "Untangling Cloth") as a safety net: finds intersections that slipped through anyway and pushes the tangled regions apart over the next steps, instead of letting them lock together for good
- [ ] Multithreading: a job system for simulation and asset loading. The cloth solver is the first work that needs more than one core
- [ ] Solver on the GPU (phase 6's compute pipelines) or on several threads (the job system above), once a sheet is too slow on one core
- [ ] Fabric properties: different stiffness along the threads (warp and weft) than across them (the bias), bending stiffness (silk vs denim), mass per area, friction; a few presets
- [ ] Optional, later: IPC or codimensional IPC (incremental potential contact) for hero garments: guaranteed intersection-free at any step size, at a much higher cost per step

**Milestone:** a sheet pinned at two corners drapes over a sphere; a sheet dropped onto itself folds into a pile with no intersections (the self-collision stress test); a silk sheet and a denim sheet fall visibly differently.

## Phase 8: Animation, UI and audio

- [ ] Parent/child entity hierarchy: attach entities to others and to skeleton joints (a weapon to a hand, a hat to a head, a turret to a vehicle). Skeletal animation is the first thing that needs it, and phase 11's first-person weapon uses it
- [ ] Skeletal animation from glTF skins: joints, skinning in the vertex shader, playing and blending clips
- [ ] Text rendering with a bitmap font (debug overlays already have a stroke font, `debugText`, good for numbers but not for real UI)
- [ ] HUD: crosshair, health bars above units, ability cooldowns. Start with flat-coloured screen-space shapes drawn after phase 6's tone mapping (no depth test, alpha blending) and a tested `worldToScreen(camera, point, windowSize)`; turn the demo's MOBA destination marker (an emissive plane for now) into a HUD ring of constant pixel size
- [ ] Debug UI and in-game stats (entity count, and phase 5's frame, tick and per-system timings)
- [ ] Key bindings: games read actions (`MoveForward`, `LockCamera`) instead of keys, and players can rebind them
- [ ] Audio with SDL3: play sounds, volume, simple 3D panning by distance and direction

**Milestone:** an animated character that walks and idles with something held in its hand, with a HUD, sounds and a debug overlay.

## Phase 9: Garments and a dressmaking game

A third kind of game, on top of phase 7's cloth: cut fabric panels, sew them into garments and see them hang and move on a body. Needs phase 8's skinned characters and phase 6's physically based materials.

**Garments**
- [ ] Sewing patterns: 2D panel outlines triangulated into cloth meshes, with UVs from the flat layout
- [ ] Seams: constraints that pull matching panel edges together until they join, starting from panels placed around the body
- [ ] Dressing an animated character: the cloth collides with the skinned body as it moves

**Rendering**
- [ ] Fabric shading: a sheen term for the soft rim highlight of cloth, on top of physically based materials
- [ ] Normal maps for weave detail (needs plain `UNORM` textures, not sRGB)

**Game**
- [ ] Pattern editor: draw and edit panel outlines, choose which edges are sewn together
- [ ] Fabric choice, colours and prints per panel

**Milestone:** a simple skirt sewn from two panels hangs on a mannequin and swings as it turns; then a garment follows an animated character as it walks.

## Phase 10: Networking

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

## Phase 11: Genre demos

**Engine**
- [ ] GPU instancing for many identical meshes (minion waves, the lamp field, asteroids): one draw call for all copies instead of one each

**FPS**
- [ ] Hitscan and projectile weapons, recoil and spread
- [ ] Health, damage, death and respawn
- [ ] First-person weapon model (attached with phase 8's hierarchy) and camera feel (bob, field of view)

**MOBA**
- [ ] Pathfinding: A* on a grid first, then a navigation mesh
- [ ] Unit selection and attack-move commands
- [ ] Abilities with cooldowns, mana and targeting (point, unit, area)
- [ ] Minion waves with simple AI, towers
- [ ] Fog of war

**Milestone:** a small online deathmatch, and a 1v1 lane with minions and towers.

## Phase 12: Orbital simulation (a solar system)

A fourth kind of world: the Sun, planets and moons at real scale, moving under gravity. Like cloth, it lives in its own module on top of the core. It needs phase 5's particles and release build but no collision, animation or networking, so it can move earlier whenever we want it sooner.

**Precision and rendering**
- [ ] Double-precision vectors (`DVec3`) for simulation positions. A float keeps about 7 significant digits, so at Neptune's distance (4.5 billion km) positions would snap in steps of hundreds of kilometres; a double's 16 digits give about a millimetre. Float `Vec3` stays for rendering and everything else
- [ ] Camera-relative rendering (floating origin): subtract the camera position from each body's position in double, then convert the small difference to float, so the GPU only ever sees numbers near zero and far-away objects do not jitter
- [ ] Depth at planetary scale: reversed-Z (far plane at depth 0, where floating-point values are densest) or a logarithmic depth buffer, so a nearby spacecraft and a planet millions of kilometres behind it both sort correctly

**Simulation**
- [ ] N-body gravity with a symplectic integrator (leapfrog / velocity Verlet). Ordinary integrators slowly pump energy into or out of an orbit, so planets spiral outwards or fall into the Sun; symplectic ones keep the energy error bounded forever. Tested: total energy stays within a small band over many orbits
- [ ] Its own simulation clock with time warp: many substeps per 120 Hz tick so a year can pass in seconds, with the step size adapting to how close bodies come to each other. A naively varied step breaks the symplectic guarantee and the energy drifts again, so the adaptivity has to keep it (for example a time-symmetric step choice, or fixed-step substepping only for the bodies in a close pass)
- [ ] "On rails" Keplerian orbits for distant bodies: the position comes from the exact ellipse at any time, with no integration error and almost no cost. A body switches to full simulation when the player comes near (and back when they leave), so a spacecraft's thrust or a close pass can change its path
- [ ] Barnes-Hut (or the fast multipole method) for thousands of bodies, such as an asteroid belt: a distant cluster pulls like a single mass at its centre, as a far galaxy does, turning the O(n²) pair sum into O(n log n). Drawn with phase 11's instancing

**Milestone:** the Sun, planets and Moon orbit stably for 100 simulated years at high time warp, and you can fly the free camera from Earth to Neptune without jitter.

## Phase 13: Later and optional

Ideas with no fixed place and nothing waiting on them: pick one up when a game needs it.

- [ ] Full rigid-body dynamics: orientation, angular velocity, mass and inertia, collision response, stacking
- [ ] Clustered shading (once there are hundreds of lights): each pixel loops only over the lights that reach its part of the view, instead of every seated light
- [ ] Shadow redraw budget: at most N shadow tiles drawn per frame, most important first, far lights refreshed less often, so switching on many lights at once or turning quickly does not cost one long frame
- [ ] Separate shadow caches for still and moving casters: keep a copy of each tile with only the still world, and draw the moving things over a copy of it each frame, so one spinning object no longer forces whole tiles to redraw
- [ ] Hardware ray tracing (RTX-class GPUs): a new Vulkan backend built on the frame description from phase 4, with an acceleration structure per mesh built once at upload. SDL_GPU has no ray-tracing support, so SDL keeps the window while the backend talks to Vulkan directly. Start with ray-traced shadows or reflections over the rasterised image, and keep the rasterised path for GPUs without ray tracing. Builds on phase 6's physically based materials
- [ ] Particle effects for visuals (sparks, smoke, ability effects)
- [ ] Level editor or scene file format
- [ ] API reference with Doxygen: a `make docs` target generating HTML into `build/docs/`, with Graphviz include and dependency diagrams (they show the source layering). Needs function comments switched from `/* */` to `/** */` so Doxygen picks them up. Complements `docs/ENGINE_GUIDE.md`, which explains concepts rather than the API
- [ ] Scripting for gameplay code
