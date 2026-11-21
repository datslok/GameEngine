# Roadmap

A from-scratch C++23 engine that can host both an **FPS** and a **MOBA**. Eventually it should also host a **dressmaking game** with real cloth physics (like Dressmaker, but more realistic), a **solar system** simulated at real scale, and a **superhero game**: flying fast enough to feel it and bringing down buildings by real physics.

The core stays genre-agnostic. Genre code (camera styles, weapons, abilities, netcode tuning, cloth and sewing, orbital mechanics) lives in modules on top.

Each phase ends with a **milestone**: something you can run and see. A phase is done when its milestone works and has tests where it makes sense.

Finished items are kept to one line, with their branch names so they can be found in `git log`.

- How each works today is in [docs/architecture/](docs/architecture/), listed in [docs/INDEX.md](docs/INDEX.md).
- When it was built, why, and what was measured are in [docs/BUILDLOG.md](docs/BUILDLOG.md).

**Principles**
- The simulation runs headless: no SDL or GPU types inside it, so it can run in tests and on a dedicated server.
- The simulation advances in fixed ticks (120 Hz). Rendering interpolates between ticks.
- One branch per feature, tests alongside the code.
- Build it yourself first to learn how it works.
  - Reach for a library when the learning is done or is not the point (Dear ImGui for debug UI, VMA for GPU memory, MikkTSpace for tangents, texture encoders, DLSS and FSR).
  - Cloth, rigid bodies, ragdolls and global illumination are built by hand: they are the point.
- Measure before optimising, and keep graphics features that cost a lot (ray tracing, GI quality) as settings that scale down to typical GPUs.

**Order of work:** phase numbers are names, kept so the docs' references hold. After phase 6 the order is 16, 19 (the docs, taken ahead of the forest), 18 (a forest showcase; its shadow and level popping wait for 21), 21 (virtual geometry and shadows), 20 (clouds and weather) and 17 (rendering), then 7 onwards, as the phases sit in this file.

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

## Phase 6: Rendering foundations, lighting and global illumination (done, Oct 18-24 2026)

Light that bounces: the biggest visual step up from today's direct-only lighting. It is done early because it is one of the two things we most want to see.

Everything here also serves later phases:

- the HDR target settles where the HUD goes,
- compute pipelines run the cloth solver,
- and physically based materials carry fabric shading.

Re-planned Oct 18 2026: the engine's own Vulkan 1.3 renderer in place of SDL_GPU before global illumination (the build log).

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

**Documentation pass**
- [x] Make the docs easy for people to read (`docs-readability`)
- [x] Bring the stale parts of `docs/ARCHITECTURE.md` up to date (`architecture-stale`)

**6d. Global illumination: DDGI** (Majercik et al. 2019 and its 2021 production follow-up; research in [docs/research/ILLUMINATION.md](docs/research/ILLUMINATION.md))

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
- [x] Sampling in `triangle.frag`: 8 corner probes, trilinear, visibility and wrap weights (`ddgi-sampling`)
- [x] Debug view: a ball at each probe showing its stored light (`ddgi-probes`)
- [x] Sky and atmosphere (Hillaire 2020): a physically based sky and air lit by the sun, which probes' escaping rays see (`sky-atmosphere`)
- [x] Night sky: the moon in its real place and phase (a second sky lit by it, its shadows) and the real stars (`night-sky`)
- [x] Eye adaptation: an exposure that follows the scene's brightness, measured as a histogram on the GPU (`eye-adaptation`)
- [x] Night vision (off by default) and the light lab's moon phase contrast (`moon-phase-contrast`)
- [x] The light lab's street: 60 shadowed lamps and lit windows, on at dusk or with K, the test ground for many lights at night (`light-lab-street`)
- [x] Cascades for large worlds: nested grids following the camera, scrolled as circular buffers, scrolled-in probes seeded from the coarser cascades (`ddgi-cascades`)
- [x] The demo's light probes: cascades round the camera, the course static, the flying lamps fast (`demo-probes`)
- [x] Up to 1024 rays a probe, the update streaming them through shared memory and rays kept only for the traced probes (`ddgi-more-rays`)
- [ ] Golden images that land on one of two pictures (`street-night`, `lamp-field`; the build log's `demo-probes` entry): look first at many lights at probe rays' hits (`keepBrightestLight` ties, the order lights reach a hit)

**6e. Image quality**
- [x] Motion vectors and temporal anti-aliasing (TAA) (`taa`)
- [x] DLSS and FSR as options, on TAA's motion vectors and jitter (`render-scale`, `fsr`, `dlss`)
  - [x] A render scale, with TAA upscaling (TAAU) (`render-scale`)
  - [x] Full-screen motion vectors and FSR 3.1, behind an upscaler setting, with its sharpening (`fsr`)
  - [x] DLSS through NVIDIA Streamline (`dlss`)
- [x] Normal map filtering (Toksvig) (`toksvig`)
- [x] Reflections: screen-space reflections, a reflection probe round the camera for what the screen does not show, and ray-traced reflections on RT GPUs (`ssr`, `reflection-probe`, `mirror-and-fps`)
  - A lattice of probes round the camera (for what the camera's own point cannot see) if gaps show.
- [x] Light reflected by mirrors: manifold next event estimation on ray-tracing GPUs (`mirrored-lights`)
- [x] Mirrored lights, the rest: one shared search, its bounce into the probes, the lit spot in reflections, metals' matte colour, rough mirrors by a Gaussian footprint (`mirrored-light-bounce`)
- [x] Ambient occlusion (GTAO) from the prepass's depth, for the small creases DDGI's coarse grid cannot resolve (`gtao`)
- [x] glTF occlusion and emissive textures (`gltf-occlusion-emissive`)
- [x] Cut-outs (glTF's alpha mask, with coverage-preserving mips) and double-sided materials, the light lab's chain-link fence (`alpha-mask`)
- [x] Glass and blended surfaces: glTF's alpha blend and thin glass (KHR_materials_transmission) by dual-source blending, sorted per object, the light lab's window pane (`glass`)
- [x] Glass under anti-aliasing: a reactive mask from the picture before and after the glass, read by TAA, FSR and DLSS (`transparency-motion`)
- [x] Tinted shadows through glass: every shadowed light in the main pass and the probes' and reflections' rays passed through the glass on the way, on GPUs with ray queries (`glass-shadows`)
- [x] Mirrored lights through glass: both legs of the path tinted (`mirrored-light-glass`)
- [x] Refraction: frosted and thick glass (`KHR_materials_volume`, `KHR_materials_ior`) from the blurred picture before the glass, the light lab's glass ball and frosted panel (`refraction`)
- [x] Volumetric fog: the air lit by every light through a froxel grid on the clusters' lists, the light lab's ground mist (`volumetric-fog`)

**Milestone** (in the light lab's building)**:**

- a room lit only through a window by the moving sun fills with soft bounce light that changes as the sun moves,
- a red wall tints the floor beside it,
- and no light leaks through the walls into the next room;
- the moon and lamps glow with bloom;
- the lamp field runs with hundreds of lights;
- edges are smooth (TAA) and metals reflect the room.

✔ (checked by the user, Oct 24 2026)

## Phase 16: Beautiful without ray tracing (done, Nov 6-13 2026)

Everything a GPU without ray tracing needs to look finished, before any more ray tracing. Re-planned on Nov 6 2026, the user's choice: DDGI on hold, phase 7 parked.

- Ray tracing stays off by default (`ray-tracing-off`), and the DDGI code stays in the engine, off by default by its own switch too; only work on it stops.
- Each feature is judged on the typical GPU setup below, not on the RTX 5080's defaults.

**Items**

- [x] Screen-space global illumination (`screen-space-gi`)
  - [x] Its noise averaged over frames and denoised before the upscalers see it, and the fill removed (`ao-temporal`)
- [x] Sky visibility: rooms dark without probes, a CPU-baked grid of how much sky each point sees (`sky-visibility`; its screen fill removed in `ao-temporal`)
  - [x] Deep rays where the sky is seen through few rays (`sky-adaptive-rays`)
- [x] The sky visibility grid as CPU light probes: its rays' hits lit, so the grid carries bounce light (designed Nov 11 2026; stage 3, lamps, moved to phase 14)
  - [x] Stage 1: the sky's bounce, per unit of the flat ambient, from each triangle's albedo, in place of the guess (`sky-bounce`)
  - Stage 2, the sun's bounce: shelved Nov 12 2026 at the user's choice, moved to phase 17 (the unmerged `sun-bounce` branch)
- [x] Planar reflections, with the mirror's own sun cascades (`planar-reflections`, on `screen-space-gi`)
  - Still optional: a sky stand-in in place of the flat colour where the probe is off or on Low; the reflection probe's missing sun shadows behind the camera without ray tracing.
- [x] Contact shadows for the sun, cut-outs passed over (`contact-shadows`)
  - [x] A crossing test in place of the fixed thickness, and the light lab's pen table (`contact-shadows`)
  - [x] For point and spot lights too: the 4 brightest shadowed ones, in the same pass, and the light lab's desk lamp (`local-contact-shadows`)
- [x] Coloured shadow maps: glass tinting shadows without ray tracing, and the light lab's stained glass (`coloured-shadow-maps`)
- [x] Mirrored lights without ray tracing: the image light for flat mirrors (`image-lights`)
- [x] Steadier volumetric fog: history read at each froxel's middle, a jitter turned per froxel (`steadier-fog`)
- [x] Materials (the rest moved on: anisotropy and skin to phase 14, sheen to phase 10, decals to phase 12)
  - [x] Clear coat: a varnish layer over any material, from glTF's KHR_materials_clearcoat, and the light lab's painted balls (`clear-coat`)
  - [x] Parallax occlusion mapping with self-shadowing from the sun, and the light lab's brick wall and cobbles (`parallax-occlusion`)
- [x] Camera finish: motion blur from TAA's velocity image, depth of field, colour grading, lens effects (all off by default, the user's choice)
  - [x] Colour grading and lens effects: white balance, the grade, vignetting, chromatic aberration, film grain (`camera-grading`)
  - [x] Motion blur: McGuire 2012's reconstruction filter from the shutter angle, off by default (`motion-blur`)
  - [x] Depth of field: a thin lens's circle of confusion and scatter-as-gather at half resolution, off by default (`depth-of-field`)
  - [x] Frosted glass through the same gather: each point behind the pane spreading by its own distance behind the glass (`frosted-gather`)
- [x] A typical GPU setup: the High preset, ray tracing off, DLSS at Quality, one switch to view it (`typical-gpu`)

**Milestone:** with the typical GPU setup, the light lab and the demo look finished by day and at night, at a frame rate a mid-range GPU holds, with no feature that needs ray queries. Met Nov 13 2026, checked by the user's eye.

---

## Phase 19: Documentation refactor (done, Nov 17 2026)

The docs cut down and kept down, taken ahead of the forest at the user's request; started from an external reviewer's plan. The rules it set (one home per fact, budgets, the workflow) live in the root `CLAUDE.md`.

**Items** (all on `docs-refactor`)

- [x] The root `CLAUDE.md` rewritten (103 KB to under 6 KB); folder `CLAUDE.md` files, path rules, skills, `docs/INDEX.md`, `docs/LAYOUT.md`
- [x] `docs/ARCHITECTURE.md` split into 51 files in `docs/architecture/`, each checked against the code
- [x] `README.md` cut from 68 KB to 12.5 KB; the guide rewritten into 11 chapters in `docs/guide/`; the build log split into parts in `docs/buildlog/`
- [x] Memory deduplicated: project rules out of memory
- [x] The checks: `testDocs` in the routine suite, `tests.exe --docs-report` for the review, the pre-commit reminder (`tools/docs_reminder.ps1`), the `docs-review` skill
- [x] Research docs moved into `docs/research/` (the user, Nov 17 2026; `docs-research-folder`)

**Milestone:** the root `CLAUDE.md` within 8 KB and readable in one sitting, every doc listed in `docs/INDEX.md`, the checks passing in the routine suite. Met Nov 17 2026.

---

## Phase 18: A jungle valley showcase ← current

A fourth game (`--game forest`): a jungle valley kilometres across with mist, sandstone pillars hundreds of metres tall, floating islands of rock and forest, and colossal machines half reclaimed by the green. Realistic: scanned CC0 plants, rocks and textures on generated pillars, islands, structures and trees, judged with the typical GPU setup on. Added Nov 13 2026 before phase 17; how it was re-planned: `docs/buildlog/phase-18-1.md` (`valley-replan`, `rough-valley`).

**Items**

- [x] Assets: 30 CC0 assets fetched by `tools/fetch_forest_assets.py` (`forest-assets`)
- [x] Ground blending: up to four layers sharpened by their height maps (`forest-assets`)
- [x] The game: terrain, clearing, path, blended ground, sun, valley mist, stand-ins (`forest-assets`)
- [x] The rough valley: rings of land to 20 km, grey pillars, floating islands, the Ring and the Monolith at real size (`rough-valley`)
- [x] The ground's paint without squares (`ground-paint`)
- [x] Distant shadows: the sun's far shadow map, 5 km round the camera (`far-shadows`)
- [x] 15 more CC0 assets (`more-assets`)
- [x] The far mist in the pillars' shadows (`fog-far-shadows`)
- [x] The sky visibility grid's cascades blended (`sky-cascade-blend`)
- [x] Pillars and floating islands: strata rock, roots and vines, hanging moss, drifting rocks (`rock-strata`, `island-roots`, `hanging-moss`, `drifting-rocks`)
- [ ] The forest: grown trees on the floor, the pillars' tops and the islands, the understorey, logs and rocks (research in `docs/research/TREES.md`, `docs/research/JUNGLE.md`)
  - [x] Grown trees (`tree-growth`)
  - [x] The jungle patch along the path: canopy and emergents (`jungle-canopy`), midstorey (`jungle-midstorey`), scanned understorey (`understorey-scans`), palms (`jungle-palms`), lianas, climbers, epiphytes (`jungle-vines`, `jungle-climbers`, `jungle-epiphytes`), light through leaves (`leaf-transmission`), wind (below)
  - [ ] The rest of the valley, keeping the sight lines to the Monolith and the Ring (the user's order, Nov 19 2026: after the structures' stage 5, before their stages 6-14; research and the user's choices in `docs/research/VALLEY.md`); each stage measured before the next
    - [x] 1. Density maps a layer over floor, walls and rim, from the terrain, with ecotopes, noise gaps and sight corridors capping the canopy; the patch rebuilt from them (`valley-maps`)
    - [ ] 2. Plants made on the GPU round the camera from the maps, by cells, every layer (Horizon's placement; the user's choice of the GPU deciding, Nov 19 2026; design in `docs/research/VALLEY.md`) (`gpu-placement`)
      - [x] a. The placement maths as a `.glsl` library mirrored in C++, exact on both (`precise`, reciprocals and the axis from the CPU, the 64-bit hash); every layer planted a box at a time, a box alone the same as the whole (`gpu-placement`)
      - [x] b1. The maps and layer records on the GPU, the placement passes a cell (candidates, spacing, standing grid a layer), cells made and dropped round the camera into fixed slots; `placement-compare` (the GPU's cells read back against `plantLayers`); nothing drawn yet
      - [x] b2. The placed trees drawn in the camera's prepass and main pass over the patch only (the user's choice, Nov 19 2026): a record a plant, a part table a variant (each part's instance and cull record in the plant's frame), culled a plant (mirrored, the camera's own choices), drawn by the passes' pipelines from a placed first instance; behind `placedTrees` (off), the entity trees hidden while on, their shadow stand-ins kept (left to me, Nov 19 2026)
      - [ ] c. Shadows, velocity, sway and the captures for placed plants; then `placedTrees` on and the patch's tree entities retired (the user's picks, Nov 19 2026: two steps, stand-ins kept, culling by cells then blocks then plants)
        - [x] c1. Placed plants in every view (camera, its velocity list, probe faces, shadow tiles drawn), each view's GPU work only over the blocks of its cells in view (each cell's box tested on the CPU); shadow tiles redrawn by the cells made or dropped and by a new table; the shadows' stand-ins and the sway in the looks
        - [x] c2. The trees always placed (`placedTrees` gone) and the patch's tree entities retired, every part of them (no sky blocker lost: trees were never static, so the sky bake never saw them); lianas stay, from the CPU's planting; the turn's sine and cosine once a vertex; the placed views' unused moving-list flag dropped
        - [ ] c3. Rays meeting placed plants (ray tracing, probes): instances the GPU writes from the placed records (option B, Nov 20 2026), when ray tracing or the far tiers need it; the GPU's lianas with them (their coarsest tubes written by a compute pass a cell, `docs/research/VINES.md`)
      - [ ] d. Every layer on the GPU and the near trees on the CPU (the user's go, Nov 20 2026; c3 parked until ray tracing or the far tiers need it)
        - [x] d1. The understorey and palms as placed looks (levels, cards, fades, thinning), their entities retired; still the patch (`gpu-understorey`)
        - [x] d2. The near trees on the CPU in cells round the player, the lianas drawn by the GPU from records a cell (the user's choice, Nov 20 2026: meshes cannot be freed; `docs/research/VINES.md`)
          - [x] d2a. The big trees planted on the CPU in 64 m cells (over the patch for now), the same maths; each crown's lianas chosen by its own random stream; the commands and the start numbered from the patch planted once (`near-tree-mirror`)
          - [x] d2b. The cells held round the player within 200 m; liana records a cell slot; one template tube at three levels bent onto the catenary by the shaders (mirrored `.glsl`), the wind worked out there; the liana meshes and the anchor table retired; holds inside the crowns' envelopes (`gpu-lianas`)
          - [x] d2c. Ivy as template cards along the lianas, dense in the crowns, every other card dropped far off (`gpu-ivy`)
          - [x] d2d. Persson's wire anti-aliasing, if far lianas flicker: not needed, no flicker (the user, Nov 20 2026)
        - [x] d3. Grass and floor cover as placed layers: a patch an 8 m lattice cell, the paint's means baked into a map, placed plants following the ground; their entity cells retired (`gpu-ground-cover`; the user's go, Nov 20 2026)
          - [x] d3a. The engine: a layer's kind and quarter turns read from a baked byte map, ground-following placed parts (lifted by the land under each vertex less the land under the plant), a region a layer, culling spheres grown by the land's relief; CPU tests and `placement-compare`
          - [x] d3b. The forest's grass and floor cover as two such layers, their entity cells retired; the frame measured before and after
      - [ ] e. The whole valley (the user's go, Nov 20 2026)
        - [ ] e1. The patch's limit gone; three sets of cells, each planting its layers and re-planting what they read: trees in 128 m cells to 1 km, the understorey and palms in 64 m to 300 m, grass and floor cover in 32 m to 100 m (the user's choice A, Nov 20 2026); plants faded in over their last cell (`valley-wide-cells`)
          - [x] e1a. The sets and their rings, every set's slots in the shared buffers, the region gone (the GPU's plants and the near trees')
          - [ ] e1c. The frame back down (the user's go, Nov 20 2026; trees to 1 km took the laptop's GPU frame from about 17 to 54 ms; research in `docs/research/VALLEY.md`, Drawing the whole valley): each step measured at the path in one hidden copy before the next
            - [x] 1. The mid trees in the middle cells (300 m): past it only the sparse tall trees (BF3's payloads, Serious Sam 4's split)
            - [x] 1b. Trees switched to their cards by size: the card's error its own texels' (2), ahead of any coarser level (the user's go, Nov 21 2026: the frame was triangles, not pixels)
            - [x] 1c. Sun cascades a mover reaches keep their movers apart, as the far map (the user's choice A, Nov 21 2026: the drifting rocks redrew whole cascades of forest, 10.6 ms 150 m up)
            - [x] 2. Trees cast into the four cascades, not the far map (the user's go, Nov 21 2026; the nearest two would have ended their shadows about 9 m away)
            - [ ] 2b. The far forest's shadow from a cheap caster in the far map (one a cell, or a canopy height field: Witcher 3, CryEngine)
            - [ ] 3. Placed plants in the probe faces only near the probe
            - [ ] 4. Sun cascades as balls round the camera in coarse steps (1/16 of a box), so turning never redraws them and walking rarely (the user's go, Nov 21 2026; `docs/research/SHADOWCACHE.md`, Cascades while the camera moves) (`cascade-steps`)
            - [ ] 5. Scrolling a stepped cascade's still copy, only the exposed strips' casters drawn (Insomniac) (`cascade-scrolling`)
          - [ ] e1b. The fade by distance over each set's last cell, by the level fades' dither
        - [ ] e2. Measured at the rim, the path and the meadow (F4), radii and levers tuned
    - [ ] 3. The far tier: one impostor a tree past about 1 km, casting into the far shadow map (`impostor-fields`)
    - [ ] 4. The canopy in the terrain's colour and normal past a few km (`far-canopy`)
    - [ ] 5. The pillars' and islands' tops planted from their own maps (`valley-tops`)
- [x] Smooth moving shadows in the far map (`far-shadow-movers`)
- [x] Staggered far cascades (`cascade-stagger`)
- [x] The frame built on every core (`frame-build-speed`)
- [x] Big copies off the frame (`frame-copies`)
- [ ] A persistent scene (research in `docs/research/PERSISTENTSCENE.md`)
  - [x] Still objects (`still-objects`)
  - [x] Still draws' records kept (`gpu-scene-persistent`)
  - [x] Lasting GPU buffers by cells (`scene-cells`)
  - [ ] Views without full scans (measured first)
  - [x] The terrain's ground materials packed once a run (`ground-material-runs`)
  - [x] Still entities announced, not searched for (`still-announced`)
- [x] Cheaper forest shadows (`understorey-shadow-cascades`)
- [x] The islands' shadows inside the cascades (`distant-occluders`)
- [x] Small plants' shadows thinned out across their last cascade, not cut at its edge about 20 m away (`shadow-thin-out`)
- [x] Small plants', climbers' and epiphytes' shadows in every cascade, so they no longer pop in about 20 m away (`small-plant-shadow-reach`)
- [x] Motion vectors written by the main pass, the separate velocity pass retired (research in `docs/research/WIND.md`, "Where the motion vectors are written") (`velocity-in-main-pass`)
- [x] Grass and ground cover (research in `docs/research/GROUNDCOVER.md`): ground-following patches (`ground-cover-patches`), meadow grass (`meadow-grass`), the forest floor (`forest-floor-cover`)
- [x] Grass about a quarter cheaper for the same cover: fewer, wider blades near, each level half the blades twice as wide (as Ghost of Tsushima's), measured first (`grass-by-pixels`)
- [ ] The structures: a military brutalist megatower over the jungle, the terraced Bastion (the user's pick, Nov 19 2026), the Ring kept as the alien relic (research, concept sheets and plan in `docs/research/STRUCTURES.md`)
  - [x] 1. Faceted slabs in the stealth-alien look, kept for the Ring (`structure-slabs`)
  - [x] 2. Panel lines and sawtooth seams: a generated seam map read in each facet's own plane (`structure-seams`)
  - [x] 3. The Bastion's mass: armoured glacis, three setbacks of fins and slots, the command head on corbels, concrete with formwork; far towers from the same kit; the strut rebuilt as an armoured bridge (`bastion-mass`)
  - [x] 4. Slit windows, light strips up the fins and floodlit walls: a procedural mask in the facet plane, far slits kept at their average (`slit-windows`)
  - [x] 5. Obstruction lights: red levels every 105 m flashing together, sprites keeping their energy with distance (`obstruction-lights`)
  - [ ] 6. Garden terraces cantilevered from the sides and gardens on the setbacks, planted by the jungle's generators, strung with lights (`bastion-terraces`)
  - [ ] 7. The crown: radome, a turning radar, phased arrays, lattice masts (`bastion-crown`)
  - [ ] 8. Weathering: stains under every ledge, rust under steel, light moss (`bastion-weathering`)
  - [ ] 9. Details: hangar bays, a landing pad, stencils, clutter at the base, a few real lights (`bastion-details`)
  - [ ] 10. Overgrowth: vines from the setbacks, soil and rubble at the foot (`bastion-overgrowth`)
  - [ ] 11. The research camp: generated modules, dome, mast, pad and floodlights, CC0 props, warm spot lights (`research-camp`)
  - [ ] 12. The Ring's segments bobbing in a wave travelling clockwise round it, a pulse riding the crest (`ring-wave`)
  - [ ] 13. The gas giant: an analytic body in the sky with ring, shadows and a dim light at night (`gas-giant`)
  - [ ] 14. Searchlight beams kilometres long through the height fog, shadowed (`searchlight-beams`)
- [ ] Layers of fog (research in `docs/research/FOGLAYERS.md`)
  - [x] Up to four fog layers, exponential or pooled; the jungle's floor mist under the canopy; the sun dimmed through them (`fog-layers`)
  - [x] Fast lights (the flashlight) outside the fog's history, fresh each frame, so the fog does not trail behind them (research `docs/research/FOGHISTORY.md`; `fog-fast-lights`)
  - [ ] Later, if other moving lights trail: the history clamped to its 6 neighbours (variance clipping)
  - [x] The floor mist off by default (`floor-mist-off`)
  - [ ] Later: local fog volumes in the froxels, then drifting noise near the camera
- [ ] Wind (research in `docs/research/WIND.md`); left: its shadow cache cost measured
  - [x] The wind field (`wind-field`), swaying plants (`wind-sway`), hanging things (`wind-hanging`), follow-ups (`wind-flutter`, `wind-chain-swing`)
  - [x] Lamps' shadows sway near the camera, redrawn every frame, so leaves near the flashlight are not blackened by their own still shadow (`sway-local-shadows`)
  - [ ] Later: a still lamp's tile caches its still casters and redraws only the swaying ones over them, as Unity HDRP's "always draw dynamic" (research `docs/research/WIND.md`, lamps' shadows)
- [x] The flashlight held close to the eye, its hand's offset in its settings, so near shadows land behind things as seen (`flashlight-hand`)
- [ ] Level of detail (research in `docs/research/LOD.md`): by error in pixels (`tree-lod`), shadows from the same levels (`shadow-lod`), the terrain's CDLOD (`terrain-lod`)
- [x] Impostors (`tree-impostors`, `impostor-switch`, `plant-impostors`)
  - [x] Cards shadowed by their own leaves: a depth atlas baked beside the normals, the sun's frames read as the plant's own shadow map, the scene's sun shadow read at the leaf seen rather than on the card, so small plants no longer glow in sun flecks (research in `docs/research/LOD.md`, "Self-shadowed impostors") (`impostor-self-shadow`)
  - [x] The scene's sun shadow on a card from points spread over its plant and averaged, one value a card, so a card at a fleck's edge is not lit whole where its mesh is mostly shadowed (probe: the leaf points lay in the fleck, the card itself half lit, its mesh shadowed) (`card-fleck-glow`)
- [ ] Pop-in: done for the trees' and palms' cards (`lod-pop-in`, `palm-thinning`); left: the trees' bark if it shows
  - [x] The scanned plants' levels cross-faded by a dither over a band of distance (`plant-fades`)
- [x] Leaf shimmer: the trees' flutter slower and in step (`tree-flutter`)
- [ ] Occlusion culling (research in `docs/research/OCCLUSION.md`)
  - [x] Measured first (`occlusion-measure`): about 2% of the GPU frame to gain
  - [ ] Two-phase culling (`occlusion-culling`), only if a scene hides more triangles
  - [ ] Only if the numbers ask: shadow casters culled by what the camera sees, solid stand-ins for dense crowns
  - [x] The camera's CPU work measured in parts (`camera-timing`)
- [ ] GPU-driven drawing (research in `docs/research/GPUDRIVEN.md`)
  - [x] 1. Indirect draws (`indirect-draws`)
  - [x] 2. Culling records and a level table on the GPU (`cull-records`)
  - [x] 3. The camera's lists built on the GPU (`gpu-cull-camera`)
  - [x] 4. Reflection probe faces and the mirror as GPU views (`gpu-cull-captures`)
  - [x] 5. Shadow tiles as GPU views (`gpu-cull-shadows`)
  - [x] 5b. The CPU path deleted (`cpu-culling-removed`)
  - [x] 6. One shared mesh buffer (`mesh-megabuffer`; research in `docs/research/MESHPOOL.md`)
  - [x] 7a. 128 m cells kept on the GPU (`scene-cells`)
  - [x] 7b. 64-slot chunks culled first (`scene-chunks`)
  - Phase 13 adds: cells loaded by distance, merged far models a cell, double coordinates, a cell test above the chunks'
- [x] Texture compression: BC7 everywhere (`texture-compression`; research in `docs/research/TEXTURECOMPRESSION.md`)
- [x] Video memory in use on F4 (`vram-in-use`)
- [x] DLSS Quality by default (`dlss-quality-default`)
- [ ] The GPU-bound frame (later, the user's choice Nov 18 2026): each pass measured (shadows while turning too), then the levers in `docs/research/WORLDSCALE.md`, coarser leaves in the far cascades among them (`docs/research/SHADOWCACHE.md`)
- [x] A loading screen while the world builds, not a black window (`loading-screen`)
- [ ] Far terrain following the camera, only if needed
- [ ] Aerial perspective from the sky's tables, only if the haze looks wrong
- [ ] Golden views, tests, packaging with the downloaded assets, docs
  - [x] The golden views' scene in the slow tests (`golden-views-slow`)

**Milestone:** with the typical GPU setup, the scale is felt from the valley's rim (pillars, floating islands and the Monolith layered into kilometres of haze), and walking down through the jungle to the Monolith's foot looks lush and alive (wind in leaves and vines, light through the canopy, mist in the valley) by day and at dusk, at a frame rate a mid-range GPU holds (measured on the laptop's RTX 4070, 8 GB).

---

## Phase 21: Virtual geometry and virtual shadow maps

Added Nov 21 2026 (the user: AAA quality; the forest's shadows popped all over from the cascades' savings, and drawing every cascade every frame cost 15-25 ms of shadows walking). Cost grows with what is on screen, not with the valley (hard rule 11). Research in `docs/research/VIRTUALGEOMETRY.md`; each stage explained and measured at the forest's spots before the next, and built after the user's go.

- [ ] 1. Clusters: meshes cut into small clusters (meshoptimizer), each culled a view (frustum, last frame's depth pyramid, facing) and drawn by mesh shaders (the user's choice, Nov 21 2026, over a compute-written index buffer); prepass and main pass first, measured
  - [x] 1a. Clusters built from a render mesh (`buildMeshClusters`): up to 64 vertices and 124 triangles, a bounding sphere and a cone of facing each; every triangle in exactly one (`clusters`)
  - [x] 1b. Clusters built when a mesh reaches the GPU and kept beside the mesh pool; culled a view on the GPU after the objects: frustum and facing, in a fixed order, mirrored in a `.glsl` library and checked by `cluster-compare` (`clusters`)
  - [x] 1c. Clusters drawn by mesh shaders (`VK_EXT_mesh_shader`) in the camera's prepass and main pass, behind Passes "Mesh clusters" (off until 1d pays), today's path for GPUs without them; lianas, cloth, grass and floor cover stay on today's path (`clusters`)
  - [ ] 1d. Two-pass occlusion: clusters hidden behind last frame's depth pyramid retested against this frame's; measured at the rim, path, trees and 157 m up (`clusters`)
- [ ] 2. Cluster levels: groups simplified to half with their borders locked, a hierarchy (meshoptimizer's `clusterlod`, a newer meshoptimizer than our 0.22), the cut chosen a view by projected error (about a pixel); the rock's, terrain's and trunks' levels and cards retired
- [ ] 3. Leaves as geometry: modelled leaves instanced along twigs and limbs (assemblies), pruned by a fixed rank inside the hierarchy with the survivors grown, clumps or voxels far (`docs/research/FARFOLIAGE.md`); wind as a rigid turn a part about its limb, stopped past a distance; the leaf cards, impostor cards and shadow stand-ins retired; the grass and floor cover stay as they are (already opaque, levels keeping their cover; the user, Nov 21 2026): stage 1 culls their patches by cluster, stage 4 decides their shadows (contact shadows instead, measured), stage 5 helps them most
- [ ] 4. Virtual shadow maps for the sun: a clipmap of virtual maps in pages round the camera, pages marked from the depth buffer, one physical pool, clusters culled and drawn a page, pages kept until something in them moves; the cascades, their steps and thinning and the far map retired; then the lamps
- [ ] 5. A visibility buffer and material pass, then a compute rasteriser for small clusters, if triangles still bound the frame
- [ ] 6. Cluster pages streamed, when scenes outgrow memory

**Milestone:** the forest walked and flown with no shadow or level popping, the sun's shadows costing what the screen needs, the frame measured on the laptop against today's.

---

## Phase 20: Clouds and weather

Added Nov 18 2026 (the user: volumetric clouds, weather differing by area as in GTA 6; looks over realism, as Red Dead Redemption 2's and The Witcher 3's skies). Research on skies and clouds in `docs/research/SKY.md`; the rest researched first, the plan explained, then the user's go.

- [ ] A sky that looks great: art controls over the physical sky as curves over the hour, one set a weather; the sun and moon art-directed
- [ ] Volumetric clouds, shadowing the ground and the fog, the cost growing with pixels
- [ ] Weather by region: a world map of cloud cover and type, rain, wind and fog, blended across borders, deterministic from the seed and the tick
- [ ] Rain and wet surfaces: rain near the camera, wet darker materials, puddles, drying

**Milestone:** from the valley's rim, a storm rolls in over one side of the valley while the other stays in sun, and walking into it the clouds, rain, wind and wet ground change together.

---

## Phase 17: Ray tracing and path tracing

The top of the quality ladder, once the path without ray tracing is finished. Always optional, off by default, for GPUs with ray queries. DDGI comes off hold here if it is still wanted. Plan notes: `docs/research/ILLUMINATION.md` (Planned), `docs/research/LUMEN.md`.

**Items**

- [ ] The sun's bounce without probes' blotches (moved from phase 16; what was tried is on `sun-bounce`)
- [ ] More hardware ray tracing: ray-traced shadows, ReSTIR, foliage solid to rays
- [ ] DDGI at the valley's scale: far cascades updated rarely
- [ ] Lumen-like global illumination: screen probes on the visible surfaces, in stages
- [ ] Path-traced mode, first as an offline reference for the golden views

**Milestone:** path-traced golden views converged offline stand beside the real-time picture, the differences measured; the path-traced mode runs live on a high-end GPU.

---

## Phase 7: Cloth simulation (parked)

The physics half of the dressmaking game: fabric that drapes, folds and piles up without passing through itself, ending with a dress sewn from two panels on a mannequin. Cloth lives in its own module on top of the core; the plan and research: `docs/research/CLOTHGPU.md` (read it first). **Parked on Nov 6 2026** (the user's choice) for phases 16 and 17; self-collision on the GPU to be rebuilt, not finished as it stands on `gpu-self-collision`.

**Items**

- [x] Job system: a work-stealing thread pool with `parallelFor` (`job-system`)
- [x] Cloth lab: a test ground with a station for each milestone, `--game clothlab` (`cloth-lab`)
- [x] Dynamic meshes: deforming meshes drawn blended between ticks (`dynamic-meshes`)
- [x] Particle store: the points as plain arrays, lumped masses, pins, the step (`cloth-particles`)
- [x] XPBD solver: substeps, compliance, stretch constraints, a hanging sheet (`xpbd-cloth`)
- [x] Dragging cloth with the mouse (`cloth-grab`)
- [x] Bending and moving pins (`cloth-bending`)
- [x] Collision with bodies: spheres, capsules and boxes, with friction (`cloth-collision`)
- [x] The solver on every core, bit for bit the one-thread result (`cloth-parallel`)
- [x] A tick budget: an overloaded simulation runs in slow motion (`tick-budget`)
- [x] Isometric bending (Bergou et al. 2006) (`isometric-bending`)
- [x] The yardstick: a 1 m sheet at four resolutions (`cloth-yardstick`)
- [x] Cloth on the GPU (`gpu-cloth-solver`, `gpu-cloth-in-game`, `cloth-gpu-timing`)
- [x] Vertex block descent with Galerkin coarse levels, opt-in (`hierarchical-cloth`, `gpu-block-descent`, `gpu-cloth-levels`, `cloth-point-lanes`)
- [x] Tethers, on by default (`cloth-tethers`)
- [x] GPU cloth ticks inside the tick budget (`gpu-tick-budget`)
- [x] The mannequin with a signed distance field (`mannequin`)
- [x] A dress sewn from two panels on the mannequin (`two-panel-dress`)
- [x] The dress-sized test: XPBD fails it, so garments need more (`dress-yardstick`)
- [x] Cheaper substeps (`cheaper-substeps`)
- [x] Self-collision on the CPU: the cloth lab's pile sheet (`cloth-self-collision`)
- [ ] Self-collision on the GPU, rebuilt in a more real-time friendly form, then on by default
- [ ] Fabric properties: a few presets (silk, denim)
- [ ] Optional, later: IPC as an offline reference for judging garments

**Milestone:**

- a sheet pinned at two corners drapes over a sphere;
- a dress sewn from two panels hangs on the mannequin;
- a sheet dropped onto itself folds into a pile with no intersections;
- a silk sheet and a denim sheet fall visibly differently.

## Phase 8: Active ragdolls

Euphoria-style characters: a simulated body held up by joint motors and kept standing by balance controllers, so a shove makes it stagger, step, fall with its hands out and get up again. The plan: layers 1-5 of `docs/research/RAGDOLLPLAN.md` (read it first); layer 6 comes with phase 9. Real-world units, as in the papers (gravity 9.81; the character controller keeps its Overwatch 17.5). Drawn as capsules until phase 9.

**Items**

- [ ] Rigid bodies: XPBD substeps with rotation, capsule contacts, shapes behind one interface for phase 15 (`rigid-bodies`)
- [ ] Joints: ball and hinge, swing-twist and soft limits, the ragdoll builder (`joints`)
- [ ] Joint drives: an inertia-scaled PD servo per joint (`joint-drives`)
- [ ] Gravity compensation by virtual forces, faded out when falling (`gravity-compensation`)
- [ ] Standing balance and stepping (`standing-balance`, `stepping`)
- [ ] Behaviours and supervisor, and the handover between capsule and ragdoll (`falling`, `behaviour-supervisor`)
- [ ] Debug view and demo: R spawns a ragdoll, E shoves what the view ray hits

**Milestone:** shove a capsule mannequin off the obstacle course's steps: it staggers, takes recovery steps, falls with its hands out and gets back up.

## Phase 9: Animation, UI and audio

- [ ] Parent/child entity hierarchy: entities attached to others and to skeleton joints
- [ ] Skeletal animation from glTF skins, skinned in compute once a frame for every view
- [ ] Ragdolls follow animation (layer 6 of `docs/research/RAGDOLLPLAN.md`)
- [ ] Text rendering with a bitmap font
- [ ] HUD: crosshair, health bars above units, ability cooldowns: screen-space shapes after tone mapping, a tested `worldToScreen`; the MOBA marker as a ring of constant pixel size
- [ ] In-game stats in the debug UI: entity count, frame, tick and per-system timings
- [ ] Key bindings: games read actions, players rebind them
- [ ] Audio with SDL3: sounds, volume, 3D panning

**Milestone:** an animated character that walks and idles with something held in its hand, with a HUD, sounds and a debug overlay.

## Phase 10: Garments and a dressmaking game

A third kind of game, on top of phase 7's cloth: cut fabric panels, sew them into garments and see them hang and move on a body. Needs phase 9's skinned characters. Plan notes: `docs/research/CLOTHGPU.md`.

**Garments**
- [ ] Garments from any panels, with darts, gathers and sleeves
- [ ] Tethers for worn garments: anchors on a moving body, distances along the sewn pattern
- [ ] If garments held by contact stretch: vertex block descent's coarse levels made cheap
- [ ] A coarse simulation mesh driving a fine render mesh
- [ ] Dressing an animated character

**Rendering**
- [ ] Weave detail with normal maps
- [ ] Fabric shading: a sheen term (KHR_materials_sheen)

**Game**
- [ ] Pattern editor: panel outlines and which edges are sewn
- [ ] Fabric choice, colours and prints per panel

**Milestone:** a garment drawn in the pattern editor hangs on the mannequin and swings as it turns; then it follows an animated character as it walks.

## Phase 11: Networking

Builds on the fixed tick and the headless simulation: commands go up, snapshots come down. Deferred until there is a playable FPS slice (phase 12); the determinism test keeps it possible.

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
- [ ] Asset cooking: an offline step saving compressed textures, vertex-cache-ordered meshes and tangents
- [ ] Cloth level of detail and sleeping, for many characters' capes and skirts

**FPS**
- [ ] Hitscan and projectile weapons, recoil and spread; projectiles hitting moving things too (a sphere swept against each shape grown by its radius)
- [ ] Health, damage, death and respawn
- [ ] Decals: bullet holes, scorch marks, signs and dirt, listed per screen tile as the lights are
- [ ] First-person weapon model and camera feel (bob, field of view)

**MOBA**
- [ ] Pathfinding: A* on a grid first, then a navigation mesh
- [ ] Unit selection and attack-move commands
- [ ] Abilities with cooldowns, mana and targeting (point, unit, area)
- [ ] Minion waves with simple AI, towers
- [ ] Fog of war

**Milestone:** a small online deathmatch, and a 1v1 lane with minions and towers.

## Phase 13: Orbital simulation (a solar system)

The Sun, planets and moons at real scale, moving under gravity, in its own module. Needs no collision, animation or networking, so it can move earlier. Plan notes: `docs/research/ORBITAL.md`.

**Precision and rendering**
- [ ] Double-precision positions (`DVec3`) for the simulation
- [ ] Camera-relative rendering in double (a floating origin)
- [ ] Depth at planetary scale

**Simulation**
- [ ] N-body gravity with a symplectic integrator
- [ ] Its own clock with time warp
- [ ] "On rails" Keplerian orbits for distant bodies
- [ ] Barnes-Hut for thousands of bodies

**Milestone:** the Sun, planets and Moon orbit stably for 100 simulated years at high time warp, and you can fly the free camera from Earth to Neptune without jitter.

## Phase 14: Later and optional

Ideas with no fixed place and nothing waiting on them: pick one up when a game needs it.

### Physics

- [ ] More rigid-body dynamics: oriented boxes, stacking, particles colliding with each other (`docs/research/DESTRUCTION.md`, Planned)
- [ ] A lathe: a spinning part turned smaller, chips flying (`lathe`; `docs/research/LATHE.md`)

### Lighting and shadows

- [ ] A software tracer for GPUs without ray tracing: a global signed distance field, sphere-marched (on hold with DDGI; `docs/research/LUMEN.md`)
- [ ] Physical light units and a camera exposure for all lights (`docs/research/ILLUMINATION.md`, Planned)
- [ ] The sky visibility grid's lamps (phase 16's stage 3)
- [ ] Anisotropic highlights (KHR_materials_anisotropy)
- [ ] Subsurface scattering for skin, when the engine has characters
- [ ] Tighter clusters, only if many lights ever cost more than a distance check each
- [ ] Shadow redraw budget, separate still and moving caches, virtual shadow maps (`docs/research/SHADOWCACHE.md`, Planned)

### Performance and scale

- [ ] Reuse per-frame memory (the demo's `CollisionScene` rebuilt on every `pick`), only if the timings point at it
- [ ] Meshlets and mesh shaders, a visibility buffer (`docs/research/WORLDSCALE.md`)
- [ ] Open-world streaming of textures and meshes, and a render graph
- [ ] The sky grid denoised, its rays aimed at openings, baked offline and streamed (`docs/research/ILLUMINATION.md`, Planned)

### Tools and the rest

- [ ] Slang trial: one shader source for SPIR-V and C++, replacing the hand-written mirrors
- [ ] Particle effects for visuals (sparks, smoke, ability effects)
- [x] Commands from the command line (`command-line-control`)
- [ ] Level editor or scene file format, every edit a command (undo on the same list)
- [ ] API reference with Doxygen (`make docs`, comments as `/** */`)
- [ ] Scripting for gameplay code

## Phase 15: Destruction and a flying superhero

A fifth kind of game (the user's idea, Oct 27 2026): fly fast enough to feel it, and bring down whole buildings that fall by real physics: chunks bonded together carry the weight down; break the right supports and it comes down floor by floor. The plan and research: `docs/research/DESTRUCTION.md` (read it first). Any time after phase 8's rigid bodies, better after phase 9's audio. Real-world units (concrete 20-40 MPa in compression).

**Items**

- [ ] Convex rigid bodies: boxes, then convex hulls (`convex-bodies`)
- [ ] Fracture: Voronoi chunks at load and the bond graph (`voronoi-fracture`)
- [ ] Blade cuts: a plane aimed in slow motion splits what it crosses (`blade-cuts`)
- [ ] The stress solver: statics on the bond graph by conjugate gradients (`stress-solver`)
- [ ] Breaking: damage, union-find for the pieces cut off (`bond-breaking`)
- [ ] On the GPU: thousands of chunks and the stress solve (`gpu-destruction`)
- [ ] Debris and dust: debris as particles, dust in the fog's grid
- [ ] Flight: hover, cruise, boost, quick stop, and the feel of speed (`flight-controller`)
- [ ] A destruction lab: a concrete frame, a brick house, a steel tower (`games/destructionlab/`)
- [ ] A city, once the lab's buildings work

**Milestone:** fly into the base of a ten-storey concrete building at the speed of sound; its load shifts, the next columns fail, and it comes down floor by floor in about three seconds, in a cloud of dust.
