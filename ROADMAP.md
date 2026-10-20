# Roadmap

A from-scratch C++20 engine that can host both an **FPS** and a **MOBA**, and eventually a **dressmaking game** with real cloth physics (like Dressmaker, but more realistic), and a **solar system** simulated at real scale. The core stays genre-agnostic; genre code (camera styles, weapons, abilities, netcode tuning, cloth and sewing, orbital mechanics) lives in modules on top.

Each phase ends with a **milestone**: something you can run and see. A phase is done when its milestone works and has tests where it makes sense.

Finished items are kept to one line, with their branch names so they can be found in `git log`. How each works today is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md); when it was built, why, and what was measured are in [docs/BUILDLOG.md](docs/BUILDLOG.md).

**Principles**
- The simulation runs headless: no SDL or GPU types inside it, so it can run in tests and on a dedicated server.
- The simulation advances in fixed ticks (120 Hz). Rendering interpolates between ticks.
- One branch per feature, tests alongside the code.
- Build it yourself first to learn how it works; reach for a library when the learning is done or is not the point (Dear ImGui for debug UI, VMA for GPU memory, MikkTSpace for tangents, texture encoders, DLSS and FSR). Cloth, rigid bodies, ragdolls and global illumination are built by hand: they are the point.
- Measure before optimising, and keep graphics features that cost a lot (ray tracing, GI quality) as settings that scale down to typical GPUs.

---

## Phase 0: Foundations (done, Sept 17-19 2026)

The original ten-step plan's steps 1-6, built on the CPU before any GPU code.

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

- [x] **GPU renderer** (`gpu-rendering`)
  - SDL_GPU with the Vulkan backend and SPIR-V shaders compiled by `glslc`
  - Depth texture, per-object uniforms
  - Fullscreen toggle (Alt+Enter)
- [x] **Shading and lighting, part of original step 7**
  - Surface normals, from the file or computed per face
  - Flat shading; smooth shading where the model provides per-vertex normals
  - One directional light with ambient
  - Materials: base colour and optional texture
- [x] **Textures and materials** (`textures-materials`)
  - Texture upload and a texture cache
  - UV conventions: OBJ bottom-left, glTF top-left
- [x] **Model loading** (`gltf-loading`)
  - OBJ loader: polygons, normals, UVs
  - glTF/GLB loader (cgltf): node hierarchy, base colour, external and embedded textures
  - Model bounds and normalization to a target size

**Milestone:** textured glTF models drawn on the GPU. ✔

## Phase 2: Core loop and input (done, Sept 27 - Oct 8 2026)

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

## Phase 3: Engine architecture (done, Oct 8 2026)

- [x] Entity/component layer (`entity-component`): generational entities, sparse-set storage, `World::each` systems
- [x] Platform split (`platform-split`): `Window`, renderer and a richer `Input`
- [x] `Game` interface (`game-interface`): the engine loop drives the game through hooks
- [x] Camera split (`camera-split`): `Camera` as a lens, separate controllers
- [x] Asset manager with handles (`asset-manager`)
- [x] Indexed meshes (`indexed-meshes`): a render format beside the import format

**Milestone:** the current demo runs as a `Game`, and a test runs its simulation for 1000 ticks with no window. ✔

## Phase 4: Rendering upgrades (done, Oct 8-9 2026)

- [x] Lights in a uniform buffer; several directional and point lights (`lights-uniform-buffer`)
- [x] Specular highlights (`specular-highlights`)
- [x] Spotlights and the demo's flashlight (`spotlights`)
- [x] Emissive materials and the moon (`moonlight`)
- [x] Optional smooth normals (`smooth-normals`)
- [x] Normal matrix on the CPU, depth range correction in the renderer (`cpu-normal-matrix`)
- [x] Trilinear and anisotropic filtering with mipmaps (`texture-filtering`)
- [x] Gamma-correct colour (`srgb-colour`)
- [x] Shadow mapping
  - [x] Shadows from every light in one atlas (`shadow-mapping`)
  - [x] Tiles sized by the light's reach (`light-priority`)
  - [x] The whole frame handed to the renderer (`frame-description`)
- [x] Debug drawing (`debug-drawing`)
- [x] Dithering (`dithering`)
- [x] Frustum culling (`frustum-culling`)
- [x] Light priority: seats and shadows go to the lights that matter most (`light-priority`)
- [x] Light and shadow follow-ups
  - [x] 1. Manual priority (`light-manual-priority`)
  - [x] 2. Fading handovers (`light-fading`)
  - [x] 3. No tiles for cube faces that see nothing (`shadow-face-culling`)
  - [x] 4. Shadow caching (`shadow-caching`)
    - [x] Shadows for all 64 seated point lights (`more-point-shadows`)
    - [x] Sharper spotlight shadows (`sharper-spot-shadows`)
  - [x] 5. Soft shadows, PCSS (`soft-shadows`)
- [x] Faster builds: object files, header dependencies, parallel builds (`faster-builds`)
- [x] Low-latency presentation (`platform-split`)

**Milestone:** a lit scene with shadows, and a toggle that draws every collider and bounding box. ✔ (bounding boxes; colliders join the F4 view in phase 5)

## Phase 5: Collision and movement (done, Oct 9-18 2026)

- [x] Settings in one place (`settings-structs`)
- [x] Shapes and exact overlap tests (`collision-shapes`)
- [x] Raycasts against shapes and triangle meshes (`raycasts`)
- [x] Mouse picking with colliders (`mouse-picking`)
- [x] Broad phase: uniform grid (`broad-phase`)
- [x] Static level collision against triangle meshes (`static-level`)
- [x] Kinematic character controller, tuned to feel like Overwatch
  - [x] The controller (`character-controller`)
  - [x] First-person body (`first-person-body`)
- [x] Particle physics and grenades (`particle-physics`)
- [x] Frame and system timing (`profiler`)
- [x] `release` build target (`release-build`)
- [x] Phase 5 documentation (`collision-guide`)

**Milestone:** walk around a small level in first person with walls, stairs and jumping, and throw a grenade that bounces. ✔

## Phase 6: Rendering foundations, lighting and global illumination ← current

Light that bounces: the biggest visual step up from today's direct-only lighting, and done early because it is one of the two things we most want to see. Everything here also serves later phases: the HDR target settles where the HUD goes, compute pipelines run the cloth solver, and physically based materials carry fabric shading.

Re-planned on Oct 18, 2026 (the decisions and reasons are in the build log): SDL_GPU replaced by the engine's own Vulkan 1.3 renderer before global illumination; work that does not touch the graphics API first (6a); DDGI traced with hardware ray queries first, a software tracer as the lower quality setting.

**Done before the re-plan**
- [x] HDR render target and tone mapping (`hdr-tone-mapping`)
- [x] HDR display output (`hdr-output`)
- [x] Bloom (`bloom`)
- [x] Physically based materials, glTF metallic-roughness (`pbr-materials`)

**6a. Groundwork that does not touch the graphics API**
- [x] Engine hygiene: validation only in debug builds, a determinism test, deferred changes in `World::each` (`engine-hygiene`)
- [x] Quaternions (`quaternions`)
- [x] Reversed-Z and camera-relative rendering (`reversed-z`)
- [x] Debug UI and settings file (`debug-ui`)
- [x] Normal maps (`normal-maps`)

**6b. The Vulkan 1.3 renderer**
- [x] Golden images (`golden-images`)
- [x] The port to Vulkan 1.3, then SDL_GPU removed (`vulkan-backend`)
- [x] GPU timestamps per pass and debug labels (`gpu-timestamps`)
- [x] Frames in flight as a setting (`frames-in-flight`)
- [x] GPU test tier (`gpu-test-tier`)

**6c. GPU scene, compute and lights**
- [x] GPU scene: storage buffers, bindless textures, instanced draws (`gpu-scene`)
- [x] Depth prepass (`depth-prepass`)
- [x] Compute pipelines (`clustered-shading`)
- [x] Clustered shading (`clustered-shading`)
- [x] Light lab, the second game (`lightlab`)
- [x] Cascaded sun shadows (`cascaded-shadows`)
- [x] Video memory in the F4 readout (`vram-readout`)
- [x] The sun's disc in the sky (`sun-disc`)
- [x] Sample distribution shadow maps: the cascades end at the farthest surface on screen (`sdsm`)

**6d. Global illumination: DDGI** ("Dynamic Diffuse Global Illumination", Majercik et al. 2019, built in the form of its 2021 production follow-up, "Scaling Probe-Based Real-Time Dynamic Global Illumination for Production"). Today light stops at the first surface it hits, so a room lit through one window is black wherever the sunbeam does not land, and the ambient light is a flat stand-in for all the light bouncing around. DDGI measures that bounced light at points in space each frame and lets every surface look it up. Research notes, exact formulas and the reference code's numbers: [docs/ILLUMINATION.md](docs/ILLUMINATION.md)
  - Needs first: the HDR target and tone mapping above (bounce light routinely adds up to more than 1 and would clip to white), compute pipelines, and the Vulkan renderer (for ray queries)
  - [x] Probe grid: probe volumes placed by each game, octahedral irradiance and distance atlases written from compute (`ddgi-probes`)
  - [x] Hardware ray queries: acceleration structures per mesh and per frame, instance masks, and a debug view (`ray-queries`)
  - [x] Quality presets set the light probes' rays: Low 32, Medium 64, High 128 (the default), Ultra 256, instead of a separate RTX preset (`gi-quality-presets`)
  - [x] Probe update in compute: 64 rays a probe on a turned Fibonacci spiral, back faces, sunlight at hits by shadow rays (`ddgi-probes`)
  - [x] Every light and infinite bounces at probe hits: the brightest point and spot lights by shadow rays, last frame's probes read at each hit (`ddgi-sampling`)
  - [x] Hysteresis blending: time constants (`ddgi-probes`), irradiance blended linearly (`ddgi-sampling`)
  - [x] Fast lights without trails: their bounce in a second irradiance atlas with a 0.05 s memory, 256 rays for probes within their reach (`ddgi-fast-lights`)
  - [x] Ray budget: probe updates per second, above which each frame traces half or a quarter of the still rays (`ddgi-ray-budget`)
  - [x] Adaptive memory: each irradiance texel restarts its average when its light changes by more than its own measured noise allows (`ddgi-adaptive-memory`)
  - [x] Probe sharing: each probe turns its rays its own way and reads its visible face neighbours' light, weighted by a setting (`ddgi-probe-sharing`)
  - [x] Scene events: the game signals big changes and the probes in their region restart their average (`ddgi-scene-events`)
  - [x] Leak prevention: Chebyshev visibility from each probe's distance moments (`ddgi-sampling`)
  - [x] Probe relocation and probe states: probes move off the static level, and only those a surface reads are traced (`ddgi-probe-relocation`)
  - [x] Sampling in `triangle.frag`: 8 corner probes, trilinear, visibility and wrap weights, the self-shadow bias; the specular part reads the probes in the mirror direction until reflections (`ddgi-sampling`)
  - [x] Debug view: a ball at each probe showing its stored light (`ddgi-probes`)
  - [x] Sky and atmosphere (Hillaire 2020): a physically based sky and air lit by the sun, which probes' escaping rays see (`sky-atmosphere`)
  - [x] Night sky: the moon in its real place and phase (a second sky lit by it, its shadows) and the real stars (`night-sky`)
  - [x] Eye adaptation: an exposure that follows the scene's brightness, measured as a histogram on the GPU (`eye-adaptation`)
  - [x] The light lab's street: 60 shadowed lamps and lit windows, on at dusk or with K, the test ground for many lights at night (`light-lab-street`)
  - [x] Cascades for large worlds: nested grids following the camera, scrolled as circular buffers, scrolled-in probes seeded from the coarser cascades (`ddgi-cascades`)
  - [x] The demo's light probes: cascades round the camera, the course static, the flying lamps fast (`demo-probes`)
  - [ ] Golden images that land on one of two pictures: the light lab's `street-night` sometimes differs from its reference by the same 73 pixels (5 beyond tolerance, up to 29 half-float steps), in a full `make golden` and on `main` before `demo-probes` too, and the demo's `lamp-field` once by 22; both have many lights at probe rays' hits, so look there first (`keepBrightestLight` ties, the order lights reach a hit, the relocation records read back a frame in flight later). Details in the build log's `demo-probes` entry
  - [ ] A software tracer for GPUs without ray tracing, as the lower quality setting: a global signed distance field round the camera (clipmaps of sparse 8^3 bricks holding a narrow band of distances, built in compute from the scene's triangles with an eikonal fill, the static level cached apart from what moves), traced by sphere marching in place of the probe rays' ray queries, its hits lit from a coarse volume of the scene's albedo and emissive times the probes' light, checked against the ray-traced result; probes only need blurry diffuse light, so coarse geometry is enough. Chosen over a voxel grid (Oct 20 2026): Epic tried voxels for Lumen (cone tracing, then bit bricks) and dropped them as leaky and slow for exactly this, and AMD's Brixelizer builds the same at run time; Lumen's numbers and robustness fixes (walls thinner than a voxel, the half-voxel expansion) in `docs/LUMEN.md`

**6e. Image quality**
- [ ] Motion vectors and temporal anti-aliasing (TAA): each pixel's movement since last frame, from `PreviousTransform` (already kept for interpolation) and last frame's camera, and a jittered projection; TAA averages over frames, which also lets soft shadows, DDGI sampling and screen-space effects use far fewer samples. Then DLSS and FSR as options (they need the Vulkan renderer's native handles)
- [ ] Normal map filtering (Toksvig): bumps averaged away in a normal map's smaller mips become extra roughness, so distant bumpy surfaces do not sparkle; alongside TAA
- [ ] Reflections: parallax-corrected reflection probes first, then screen-space reflections, so metals and wet surfaces reflect their surroundings instead of the uniform-sky stand-in; ray-traced reflections later as an RT setting (phase 14)
- [ ] Ambient occlusion (GTAO) from the prepass's depth, for the small creases DDGI's coarse grid cannot resolve
- [ ] glTF occlusion and emissive textures (the loader reads only the factors today)
- [ ] Alpha test and transparency: glTF's `alphaMode` (mask for foliage and fences, blend for glass) and a sorted transparent pass after the opaque scene
- [ ] Volumetric fog on the cluster grid: light scattering in the air, so beams and lamps light the fog around them

**Milestone** (in the light lab's building)**:** a room lit only through a window by the moving sun fills with soft bounce light that changes as the sun moves, a red wall tints the floor beside it, and no light leaks through the walls into the next room; the moon and lamps glow with bloom; the lamp field runs with hundreds of lights; edges are smooth (TAA) and metals reflect the room.

## Phase 7: Cloth simulation

The physics half of the dressmaking game, early because it is the other thing we most want to see: fabric that drapes, folds and piles up without passing through itself. Builds on phase 5's particle store and collision shapes and phase 6's compute pipelines. The CPU solver does not depend on the renderer, so it can start earlier (any time after quaternions) if we want cloth sooner. Cloth code lives in its own module on top of the core, like the FPS and MOBA code. Garments, sewing and the game itself follow in phase 10, once phase 9 has skinned characters to dress.

- [ ] Job system (moved to the start of the phase): a work-stealing thread pool with `parallelFor` first, for simulation, frame building, culling and asset decoding. The cloth solver is the first work that needs more than one core
- [ ] Dynamic meshes: a GPU upload path for meshes whose vertices change every frame, with normals recomputed each time (today a mesh is uploaded once and never changes)
- [ ] Particle store for cloth: the sheet's vertices as plain arrays (positions, previous positions, velocities, inverse masses, with 0 for pinned points), contiguous for the cache and for upload to the GPU, inside the cloth component, reusing phase 5's per-particle functions (`fly`, `bounceVelocity`)
- [ ] XPBD solver (extended position-based dynamics): particles integrated with gravity, then constraints project their positions back towards valid states. Each constraint has a compliance (inverse stiffness), so nearly unstretchable fabric stays stable at the 120 Hz tick, where stiff springs would need a tiny timestep. Use many substeps (10-20 per tick, one constraint iteration each) rather than many iterations of one big step: small steps converge faster for the same work, so fabric stretches less (Macklin et al. 2019, "Small Steps in Physics Simulation")
- [ ] Cloth constraints: stretch (edge lengths), bending (angle between neighbouring triangles), pinning (fixed points, for testing and for hangers)
- [ ] Collision with bodies: spheres and capsules first (phase 5's shapes), then a signed distance field around a mannequin, built in compute (it can serve later as DDGI's software tracer and for distance-field AO); friction between cloth and body
- [ ] Self-collision, so folds and layers never pass through each other. A broad phase (a BVH or spatial hash) only finds nearby pairs; cloth is so thin that a fast-moving layer can jump right through another between two substeps, so it also needs continuous collision detection (vertex-triangle and edge-edge pairs checked along their motion during the step) and a cloth thickness that keeps layers apart
- [ ] Untangling pass (Baraff, Witkin & Kass 2003, "Untangling Cloth") as a safety net: finds intersections that slipped through anyway and pushes the tangled regions apart over the next steps, instead of letting them lock together for good
- [ ] Solver on the GPU (phase 6's compute pipelines) or on several threads (the job system above), once a sheet is too slow on one core, checked against the CPU solver
- [ ] Fabric properties: different stiffness along the threads (warp and weft) than across them (the bias), bending stiffness (silk vs denim), mass per area, friction; a few presets
- [ ] Optional, later: IPC or codimensional IPC (incremental potential contact) for hero garments: guaranteed intersection-free at any step size, at a much higher cost per step

**Milestone:** a sheet pinned at two corners drapes over a sphere; a sheet dropped onto itself folds into a pile with no intersections (the self-collision stress test); a silk sheet and a denim sheet fall visibly differently.

## Phase 8: Active ragdolls

Euphoria-style characters: a simulated body held up by joint motors and kept standing by balance controllers, so a shove makes it stagger, step, fall with its hands out and get up again. Layers 1-5 of the plan in `docs/RAGDOLLPLAN.md` (read it first: it holds the papers' formulas and numbers and how each layer fits the engine); layer 6, following animation clips, comes in phase 9 with skeletal animation. Placed straight after cloth because the rigid-body solver is phase 7's XPBD with rotation and inertia added; quaternions come from phase 6a. Real-world values throughout, as in the papers: gravity 9.81 for ragdolls (the character controller keeps its Overwatch 17.5), SI masses, torques and push forces, so the papers' results can be reproduced. Until phase 9 the body is drawn as its capsules, a mannequin.

- [ ] Rigid bodies (`rigid-bodies`, moved here from phase 14's rigid-body dynamics): a `RigidBody` component (mass, body-frame inertia, velocity, angular velocity, a compound of capsules) and an XPBD substep solver (Müller et al. 2020; 10 substeps per tick, 1.2 kHz), with gyroscopic torque, capsule contacts against the static level from `contact(capsule, triangle)`, restitution 0 and friction 0.8. Capsules only: there is no oriented box. `Mesh::capsule` to draw them. Tests: momentum and angular momentum conserved in flight, the tennis-racket theorem's tumble, a dropped capsule rests without jitter, and the same run twice is bit-identical
- [ ] Joints (`joints`): ball and hinge `Joint`s with swing-twist limits and soft limits, capsule-against-capsule self-collision between unjoined parts, and the ragdoll builder from `RagdollBodySettings` (1.8 m and 70 kg by default, anthropometric proportions, feet as two parallel capsules for a flat sole). Each part is an entity with a capsule `Collider`, so picking finds ragdolls with no new code
- [ ] Joint drives (`joint-drives`): an inertia-scaled PD servo per joint, stiffness as a natural frequency and damping as a damping ratio (GTA V's units), applied as equal and opposite torques on the two bodies, capped, and scaled by a global strength and health
- [ ] Gravity compensation (`gravity-compensation`): feed-forward torques from virtual forces (τ = Jᵀ F) that hold the body up, so the drives stay soft; faded out when the centre of mass leaves the feet's support area, so a falling body goes limp
- [ ] Standing balance and stepping (`standing-balance`, `stepping`): the torso held upright in world space with the stance hip taking the balance of the torques (SIMBICON), foot placement from the inverted-pendulum capture point (Coros 2010), two-bone IK, a step state machine, and failure detection. Headless test: a 300 N push for 0.1 s at the chest leaves it standing 3 s later
- [ ] Behaviours and supervisor (`falling`, `behaviour-supervisor`): behaviours that score the situation and write targets for the body parts they own (balance, stagger, catch fall, arms windmill, head look, get up), run by a supervisor; handover between a character's kinematic capsule and its ragdoll and back
- [ ] Debug view and demo: R spawns a ragdoll in front of the camera, E shoves what the view ray hits; F4 draws the capsules, joint frames, centre of mass, support area, next foot target and the active behaviours

**Milestone:** shove a capsule mannequin off the obstacle course's steps: it staggers, takes recovery steps, falls with its hands out and gets back up.

## Phase 9: Animation, UI and audio

- [ ] Parent/child entity hierarchy: attach entities to others and to skeleton joints (a weapon to a hand, a hat to a head, a turret to a vehicle). Skeletal animation is the first thing that needs it, and phase 12's first-person weapon uses it
- [ ] Skeletal animation from glTF skins: joints, playing and blending clips, and skinning in a compute shader once per frame into a vertex buffer that the main pass, every shadow view, ray tracing and cloth collision all read (skinning in the vertex shader would repeat the work in every shadow tile)
- [ ] Ragdolls follow animation (layer 6 of `docs/RAGDOLLPLAN.md`): a table from skeleton bones to ragdoll bodies, clip poses as the joint drives' targets (a powered ragdoll), skinned characters drawn from the bodies, softening and shaped impulses on a hit, then a smooth blend back into a matching clip or get-up animation
- [ ] Text rendering with a bitmap font (debug overlays already have a stroke font, `debugText`, good for numbers but not for real UI)
- [ ] HUD: crosshair, health bars above units, ability cooldowns. Start with flat-coloured screen-space shapes drawn after phase 6's tone mapping (no depth test, alpha blending) and a tested `worldToScreen(camera, point, windowSize)`; turn the demo's MOBA destination marker (an emissive plane for now) into a HUD ring of constant pixel size
- [ ] In-game stats in the debug UI (phase 6a): entity count, and phase 5's frame, tick and per-system timings
- [ ] Key bindings: games read actions (`MoveForward`, `LockCamera`) instead of keys, and players can rebind them
- [ ] Audio with SDL3: play sounds, volume, simple 3D panning by distance and direction

**Milestone:** an animated character that walks and idles with something held in its hand, with a HUD, sounds and a debug overlay.

## Phase 10: Garments and a dressmaking game

A third kind of game, on top of phase 7's cloth: cut fabric panels, sew them into garments and see them hang and move on a body. Needs phase 9's skinned characters and phase 6's physically based materials.

**Garments**
- [ ] Sewing patterns: 2D panel outlines triangulated into cloth meshes, with UVs from the flat layout
- [ ] Seams: constraints that pull matching panel edges together until they join, starting from panels placed around the body
- [ ] Dressing an animated character: the cloth collides with the skinned body as it moves

**Rendering**
- [ ] Fabric shading: a sheen term for the soft rim highlight of cloth, on top of physically based materials
- [ ] Weave detail with phase 6a's normal maps

**Game**
- [ ] Pattern editor: draw and edit panel outlines, choose which edges are sewn together
- [ ] Fabric choice, colours and prints per panel

**Milestone:** a simple skirt sewn from two panels hangs on a mannequin and swings as it turns; then a garment follows an animated character as it walks.

## Phase 11: Networking

Builds on the fixed tick and the headless simulation. Commands go up, snapshots come down. Deferred until there is a playable FPS slice (phase 12) worth playing together; the deterministic, headless simulation keeps it possible, and phase 6a's determinism test keeps it that way.

- [ ] Serialization of commands and snapshots into compact byte buffers
- [ ] UDP sockets, connections, and a small reliability layer for messages that must arrive
- [ ] Dedicated server build that runs the simulation headless
- [ ] Authoritative server: clients send commands, the server simulates and sends snapshots
- [ ] Snapshot interpolation for other players' entities
- [ ] Client-side prediction and reconciliation for the local player
- [ ] Lag compensation: the server rewinds hitboxes to what the shooter saw (FPS)
- [ ] Simulated latency and packet loss for testing

**Milestone:** two clients move around the same level through a dedicated server, with smooth movement under 100 ms of simulated latency.

## Phase 12: Genre demos

**Engine**
- [ ] Asset cooking (before large scenes): an offline step in `tools/` that turns source models and images into ready-to-load files: textures compressed to BC7 (colour) and BC5 (normal maps), about a quarter of the memory; meshes in vertex-cache order with LODs (meshoptimizer); tangents precomputed
- [ ] Level of detail: simpler meshes for distant objects, chosen per view (instancing itself moved to phase 6c)

**FPS**
- [ ] Hitscan and projectile weapons, recoil and spread. Projectiles and grenades (phase 5's particles) must also hit moving things, characters and props, not only the static level: sweep the sphere against the moving colliders' shapes too (a sphere or capsule is a raycast against the shape grown by the radius, like `sweepSphere`; a box needs a rounded-box sweep)
- [ ] Health, damage, death and respawn
- [ ] First-person weapon model (attached with phase 9's hierarchy) and camera feel (bob, field of view)

**MOBA**
- [ ] Pathfinding: A* on a grid first, then a navigation mesh
- [ ] Unit selection and attack-move commands
- [ ] Abilities with cooldowns, mana and targeting (point, unit, area)
- [ ] Minion waves with simple AI, towers
- [ ] Fog of war

**Milestone:** a small online deathmatch, and a 1v1 lane with minions and towers.

## Phase 13: Orbital simulation (a solar system)

A fourth kind of world: the Sun, planets and moons at real scale, moving under gravity. Like cloth, it lives in its own module on top of the core. It needs phase 5's particles and release build but no collision, animation or networking, so it can move earlier whenever we want it sooner.

**Precision and rendering**
- [ ] Double-precision vectors (`DVec3`) for simulation positions. A float keeps about 7 significant digits, so at Neptune's distance (4.5 billion km) positions would snap in steps of hundreds of kilometres; a double's 16 digits give about a millimetre. Float `Vec3` stays for rendering and everything else; phase 5's per-particle functions (`fly`, `bounceVelocity`) become templates over the vector type
- [ ] Camera-relative rendering in double (floating origin): phase 6a makes rendering camera-relative in float; here the subtraction happens in double before the small difference becomes a float, so the GPU only ever sees numbers near zero and far-away objects do not jitter
- [ ] Depth at planetary scale: phase 6a's reversed-Z, plus a logarithmic depth or split near and far passes if a nearby spacecraft and a planet millions of kilometres behind it still do not both sort correctly

**Simulation**
- [ ] N-body gravity with a symplectic integrator (leapfrog / velocity Verlet). Ordinary integrators slowly pump energy into or out of an orbit, so planets spiral outwards or fall into the Sun; symplectic ones keep the energy error bounded forever. Tested: total energy stays within a small band over many orbits
- [ ] Its own simulation clock with time warp: many substeps per 120 Hz tick so a year can pass in seconds, with the step size adapting to how close bodies come to each other. A naively varied step breaks the symplectic guarantee and the energy drifts again, so the adaptivity has to keep it (for example a time-symmetric step choice, or fixed-step substepping only for the bodies in a close pass)
- [ ] "On rails" Keplerian orbits for distant bodies: the position comes from the exact ellipse at any time, with no integration error and almost no cost. A body switches to full simulation when the player comes near (and back when they leave), so a spacecraft's thrust or a close pass can change its path
- [ ] Barnes-Hut (or the fast multipole method) for thousands of bodies, such as an asteroid belt: a distant cluster pulls like a single mass at its centre, as a far galaxy does, turning the O(n²) pair sum into O(n log n). Drawn with phase 6c's instancing

**Milestone:** the Sun, planets and Moon orbit stably for 100 simulated years at high time warp, and you can fly the free camera from Earth to Neptune without jitter.

## Phase 14: Later and optional

Ideas with no fixed place and nothing waiting on them: pick one up when a game needs it.

- [ ] More rigid-body dynamics, extending phase 8's solver (bodies, joints and capsule contacts come with the ragdolls): oriented boxes with box contacts (separating axes), stacking and piles of crates. This includes particles colliding with each other (grenades knocking each other about), left out of phase 5 because piles need this phase's iterative contact solver: find pairs with the uniform grid over each particle's swept box, detect the touch by sweeping in relative motion (a ray against a sphere of the summed radii, so fast crossings are not missed), and respond by running the relative velocity through `bounceVelocity` and sharing the change by mass (an equal and opposite impulse, so momentum is conserved), with the two materials' restitution and friction averaged
- [ ] Physical light units and a camera exposure for all lights. Emissive surfaces and the lights they stand for could then be linked physically (the light a surface receives from the moon is the moon's brightness times the solid angle it covers), with the exposure mapping the real dynamic range (the moon disc is 10,000+ times brighter than moonlit ground) to the screen. Until then the demo's moon glow and moonlight, and the light lab's sun and sky, are separate artistic dials
- [ ] Reuse per-frame memory: the frame's draw list, the shadow tile lists and the debug lines are rebuilt in fresh vectors every frame; keep and clear them instead. The demo rebuilds its `CollisionScene` on every `pick` (hover and a click can both pick in one frame); rebuild it once per frame. Minor each, so only if phase 6b's timings point at them (the renderer timing breakdown, cheaper caster culling and instancing that used to be listed here moved to phases 6b and 6c)
- [ ] Tighter clusters: depth bounds per tile from the prepass and smaller far tiles, since far clusters list 16 or more lamps where 4-8 reach; only if many lights ever cost more than a distance check each
- [ ] Shadow redraw budget: at most N shadow tiles drawn per frame, most important first, far lights refreshed less often, so switching on many lights at once or turning quickly does not cost one long frame
- [ ] Separate shadow caches for still and moving casters: keep a copy of each tile with only the still world, and draw the moving things over a copy of it each frame, so one spinning object no longer forces whole tiles to redraw
- [ ] More hardware ray tracing in the Vulkan renderer (phase 6b; DDGI's probe rays come first, in 6d): ray-traced shadows (exact soft shadows from area lights), ray-traced reflections, then ReSTIR-style sampling of many lights or a path-traced reference mode. Ray tracing is always optional: settings in `RenderSettings` (so the debug UI and a player's options menu can reach them) switch each ray-traced feature on or off while the game runs, falling back to shadow maps, reflection probes and the software-traced probes. Off by default and greyed out on GPUs without ray tracing (detected at start-up); the game is designed, tuned and tested on the non-ray-traced path first, so it looks right without it
- [ ] Lumen-like global illumination (as in Unreal Engine 5; research, numbers and a staged plan in `docs/LUMEN.md`, read Oct 20 2026): probes placed on the visible surfaces themselves, one every 16 pixels plus more at depth and normal edges, 8x8 octahedral, each tracing its own rays, filtered 3x3 in probe space and over time, so light near a surface is what that surface sees. Fixes probe parallax: a probe sits a little off the surface and sees what the surface cannot, so beside a lit doorway at night the outside wall glows within a probe spacing of the opening (found Oct 20 2026: the lamp-lit jambs and room seen at grazing angles by the probes in front of the wall; wider on Low, narrower on Ultra), and the back room's ceiling glows beside its doorway; also indirect shadows smaller than the probe spacing. In stages: (1) prerequisites: 6e's motion vectors and TAA (which average away the probes' jittered placement and directions), a normal target in the depth prepass, a hierarchical depth buffer; (2) diffuse screen probes with full-length ray queries, hits shaded as the probe rays' are (no surface cache needed with hardware rays), the main pass reading a per-pixel GI image; (3) importance sampling from last frame's probes and the BRDF; (4) short-range AO to 16 pixels after the temporal filter; (5) reflections from the same rays, rough surfaces from the probes. DDGI's cascades stay as the far field (Lumen's world radiance cache) and as the Low setting; presets probes every 32, 16 and 8 px (Lumen's ladder, each level about half the next's cost). Estimated 1-2 ms on the RTX 5080 at High, to be measured
- [ ] Path-traced mode: every pixel traces random light paths through the scene with several bounces, which gets every shadow, bounce, reflection and soft light right with nothing approximated. Real time only with one or two paths a pixel, so it needs ReSTIR's reuse of good samples across pixels and frames and a strong denoiser (and upscaling); high-end GPUs only. Worth it first as a reference: golden views path-traced to convergence offline show what the real-time lighting should look like, so its errors (probe parallax, leaks, missing occlusion) can be measured rather than judged by eye
- [ ] Virtual shadow maps (as in Unreal Engine 5) for the sun, in place of cascades or beside them as the top setting: one huge virtual shadow map (16384 x 16384 per clipmap level, nested levels each twice the last's size around the camera) cut into 128 x 128 pages, of which only those that pixels on screen actually look up are drawn, into a pool of physical pages, and kept between frames until something in them moves. That gives about one shadow texel per screen pixel at every distance, with no cascade seams, and caching makes still scenes nearly free. Needs: a pass marking the pages the depth buffer touches, a page table the shaders read through, GPU-driven culling and drawing per page, and invalidation from moving casters. A large project; after DDGI (6d) and TAA (6e), and only if ray-traced sun shadows turn out too slow on typical GPUs
- [ ] GPU-driven rendering beyond phase 6c: culling in compute against each view and last frame's depth (Hi-Z occlusion culling), writing indirect draws with a GPU-written count (bounds go into the GPU instance records then); meshlets (meshoptimizer) and mesh shaders where the GPU has them; a visibility buffer if scenes ever get dense enough to need it
- [ ] Open-world scale: streaming textures and meshes in and out by distance, impostors for far objects, and a render graph that orders passes and places barriers from what each pass reads and writes, once the hand-placed barriers of phase 6b no longer keep up
- [ ] Slang trial: one shader source compiled to both SPIR-V and C++, which could replace the hand-written CPU mirrors of the shader libraries
- [ ] Particle effects for visuals (sparks, smoke, ability effects)
- [ ] Level editor or scene file format
- [ ] API reference with Doxygen: a `make docs` target generating HTML into `build/docs/`, with Graphviz include and dependency diagrams (they show the source layering). Needs function comments switched from `/* */` to `/** */` so Doxygen picks them up. Complements `docs/ENGINE_GUIDE.md`, which explains concepts rather than the API
- [ ] Scripting for gameplay code
