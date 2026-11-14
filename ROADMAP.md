# Roadmap

A from-scratch C++23 engine that can host both an **FPS** and a **MOBA**. Eventually it should also host a **dressmaking game** with real cloth physics (like Dressmaker, but more realistic), a **solar system** simulated at real scale, and a **superhero game**: flying fast enough to feel it and bringing down buildings by real physics.

The core stays genre-agnostic. Genre code (camera styles, weapons, abilities, netcode tuning, cloth and sewing, orbital mechanics) lives in modules on top.

Each phase ends with a **milestone**: something you can run and see. A phase is done when its milestone works and has tests where it makes sense.

Finished items are kept to one line, with their branch names so they can be found in `git log`.

- How each works today is in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).
- When it was built, why, and what was measured are in [docs/BUILDLOG.md](docs/BUILDLOG.md).

**Principles**
- The simulation runs headless: no SDL or GPU types inside it, so it can run in tests and on a dedicated server.
- The simulation advances in fixed ticks (120 Hz). Rendering interpolates between ticks.
- One branch per feature, tests alongside the code.
- Build it yourself first to learn how it works.
  - Reach for a library when the learning is done or is not the point (Dear ImGui for debug UI, VMA for GPU memory, MikkTSpace for tangents, texture encoders, DLSS and FSR).
  - Cloth, rigid bodies, ragdolls and global illumination are built by hand: they are the point.
- Measure before optimising, and keep graphics features that cost a lot (ray tracing, GI quality) as settings that scale down to typical GPUs.

**Order of work:** phase numbers are names, kept so the docs' references hold. After phase 6 the order is 16, 18 (a forest showcase) and 17 (rendering), then 7 onwards, as the phases sit in this file.

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

Re-planned on Oct 18, 2026 (the decisions and reasons are in the build log):

- SDL_GPU replaced by the engine's own Vulkan 1.3 renderer before global illumination;
- work that does not touch the graphics API first (6a);
- DDGI traced with hardware ray queries first, a software tracer as the lower quality setting (moved to phase 14 on Oct 23 2026).

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

**6d. Global illumination: DDGI** ("Dynamic Diffuse Global Illumination", Majercik et al. 2019, built in the form of its 2021 production follow-up, "Scaling Probe-Based Real-Time Dynamic Global Illumination for Production").

Today light stops at the first surface it hits, so a room lit through one window is black wherever the sunbeam does not land, and the ambient light is a flat stand-in for all the light bouncing around. DDGI measures that bounced light at points in space each frame and lets every surface look it up.

Research notes, exact formulas and the reference code's numbers: [docs/ILLUMINATION.md](docs/ILLUMINATION.md)

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
- [x] Night vision (off by default) and the light lab's moon phase contrast (`moon-phase-contrast`)
- [x] The light lab's street: 60 shadowed lamps and lit windows, on at dusk or with K, the test ground for many lights at night (`light-lab-street`)
- [x] Cascades for large worlds: nested grids following the camera, scrolled as circular buffers, scrolled-in probes seeded from the coarser cascades (`ddgi-cascades`)
- [x] The demo's light probes: cascades round the camera, the course static, the flying lamps fast (`demo-probes`)
- [x] Up to 1024 rays a probe, the update streaming them through shared memory and rays kept only for the traced probes (`ddgi-more-rays`)
- [ ] Golden images that land on one of two pictures
  - The light lab's `street-night` sometimes differs from its reference by the same 73 pixels (5 beyond tolerance, up to 29 half-float steps), in a full `make golden` and on `main` before `demo-probes` too.
  - The demo's `lamp-field` by 22 (several runs on Oct 22 2026) and its `floodlights` scene image once.
  - Both have many lights at probe rays' hits, so look there first (`keepBrightestLight` ties, the order lights reach a hit, the relocation records read back a frame in flight later).
  - Details in the build log's `demo-probes` entry

**6e. Image quality**
- [x] Motion vectors and temporal anti-aliasing (TAA) (`taa`)
- [x] DLSS and FSR as options, on TAA's motion vectors and jitter (`render-scale`, `fsr`, `dlss`)
  - [x] A render scale, with TAA upscaling (TAAU) (`render-scale`)
  - [x] Full-screen motion vectors and FSR 3.1, behind an upscaler setting, with its sharpening (`fsr`)
  - [x] DLSS through NVIDIA Streamline, after a trial with MinGW (`dlss`)
  - With TAA in place, soft shadows, DDGI sampling and screen-space effects can use far fewer samples and let the history average them.
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
- [x] A typical GPU setup: the High preset, ray tracing off, DLSS at Quality (FSR, else TAA, where DLSS is missing), one switch, command or launch option to view it; `preset` as a command; ray-traced settings greyed out on GPUs without ray queries (`typical-gpu`)

**Milestone:** with the typical GPU setup, the light lab and the demo look finished by day and at night, at a frame rate a mid-range GPU holds, with no feature that needs ray queries. Met Nov 13 2026, checked by the user's eye. The fog's history in seconds was dropped: a fixed share a frame trails at low frame rates where one in seconds would shimmer, the worse of the two.

---

## Phase 18: A jungle valley showcase ← current

A fourth game (`--game forest`) showing everything phases 6 and 16 built, at a scale meant to be felt: a jungle valley kilometres across with mist lying in it, sandstone pillars hundreds of metres tall, islands of rock and forest floating above it, and colossal machines built by someone long gone, half reclaimed by the green. Realistic, not stylised: scanned CC0 plants, rocks and textures on generated pillars, islands, structures and trees, judged with the typical GPU setup on.

- Added Nov 13 2026 at the user's request, before phase 17, as a 400 m forest with a clearing.
- Re-planned the same day after concept art and the user's references (floating mountains, karst pillars, overgrown megastructures): "something large and epic, i want the scale to be felt huge", with floating islands.
- The scale is mostly an engine problem (drawing less far away), so the level of detail, impostor, occlusion culling and texture compression items moved here from phases 12 and 14, and a rough valley comes first to measure what is needed.
- Re-ordered Nov 14 2026 after the rough valley (the user's look: the haze turns blue, 7.4 ms of GPU with the typical setup, "looks a bit square in some places"): the ground's paint, then distant shadows, then the content; the scale items wait until the content makes the frame heavy.

**Items**

- [x] Assets: 30 CC0 assets fetched by `tools/fetch_forest_assets.py` (`forest-assets`)
- [x] Ground blending: up to four layers sharpened by their height maps (`forest-assets`)
- [x] The game: terrain, clearing, path, blended ground, sun, valley mist, stand-ins (`forest-assets`)
- [x] The rough valley: the land in rings to 20 km, grey pillars, floating islands, the Ring and the Monolith at real size (`rough-valley`)
- [x] The ground's paint without squares: a texture per ring, turned noise, cubic reads (`ground-paint`)
- [ ] Distant shadows: the sun's shadows reaching the far pillars, islands and towers (they stop at 200 m today, so everything past it is lit on all sides) without blurring the near ones: a far cascade of its own or baked shading
- [ ] More assets: jungle plants (broad leaves, bromeliads, palms or tree ferns), banded sandstone for the pillars, weathered concrete and panels for the structures, a vine leaf atlas, hanging moss; a CC0 survey and a shortlist for the user first, as for the first 30
- [ ] The pillars and floating islands, generated
  - Sandstone pillars: wobbling, tapered columns with ledges, streaks and a crown of trees, in the scanned rock.
  - Floating islands: a rocky underside tapering to a point, a forested top, roots and vines hanging below, smaller rocks drifting round them.
- [ ] The forest: trees grown by the engine (generated trunks and branches in scanned bark, scanned leaf atlases on cut-out cards: Poly Haven's scanned trees are 2-17 million triangles each), the jungle understorey, logs and rocks, scattered by a seeded Poisson disk
- [ ] Grass and ground cover: blades merged into one mesh per 8 x 8 m patch, thinning with distance
- [ ] The structures, half reclaimed
  - The Monolith: a 600 m plated slab with setbacks, a truss bridge to a pillar, lit slots and beacons. The Ring: 400 m across, on edge, sunk to its waist. Far towers in the haze.
  - Big searchlights sweeping the land (the user, Nov 14 2026): spot lights with shadows near the camera, and for beams kilometres long, more than the fog's 100 m grid can light, a beam of their own through the height fog (its closed form along the cone).
  - Parallax hull plates, clear-coated paint, chrome pipes, glowing strips with real lights, searchlights sweeping beams through the mist, vines hanging hundreds of metres.
- [ ] Wind: a field of travelling gusts bending stems like cantilevers and fluttering leaves and vines, identical in the depth prepass, main pass, shadows, velocity pass (last frame's wind too) and captures; the shadow cache's cost measured
- [ ] Level of detail (moved from phase 12; when the trees make the frame heavy: the rough valley ran at 7.4 ms of GPU with the typical setup): simpler meshes for distant objects, chosen per view; generated meshes (pillars, islands, structures, trees) made at several resolutions by their own code, scanned ones simplified (meshoptimizer)
- [ ] Impostors (moved from phase 14): far trees and plants as cards baked from their full meshes from several directions, so a valley's canopy of tens of thousands of trees costs little
- [ ] Occlusion culling (moved from phase 14): culling in compute against last frame's depth pyramid (Hi-Z), so what hides behind the Monolith or a pillar is not drawn
- [ ] Texture compression (moved from phase 12's asset cooking): BC7 colour and BC5 normal maps, about a quarter of the memory, so the plants, rock and metals fit the laptop's 8 GB
- [ ] Far terrain following the camera, only if needed: the rough valley's fixed rings reach 20 km round the walk
- [ ] Aerial perspective: the air between the camera and far scenery from the sky's own tables (Hillaire 2020's aerial perspective volume), so distant things turn blue by distance; the fog's grid near, this beyond it (only if the haze looks wrong later: the rough valley showed it already turns blue)
- [ ] Golden views, tests, packaging with the downloaded assets, docs

**Milestone:** with the typical GPU setup, the scale is felt from the valley's rim (pillars, floating islands and the Monolith layered into kilometres of haze), and walking down through the jungle to the Monolith's foot looks lush and alive (wind in leaves and vines, light through the canopy, mist in the valley) by day and at dusk, at a frame rate a mid-range GPU holds (measured on the laptop's RTX 4070, 8 GB).

---

## Phase 17: Ray tracing and path tracing

The top of the quality ladder, once the path without ray tracing is finished. Always optional, off by default, for GPUs with ray queries. DDGI comes off hold here if it is still wanted.

**Items**

- [ ] The sun's bounce without probes' blotches (moved here from phase 16 on Nov 12 2026, the user's choice: bounced sunlight is for the ray-traced path)
  - With ray tracing the probes carry it already; this is for rooms lit only by sunlight bounced off a patch next door.
  - Tried on the CPU grid, on the unmerged `sun-bounce` branch: a shadow ray from every hit (blotchy, 4 times the bake's time) and aiming at sunlit spots found down the sun's beam (smoother, 2.5 times; spikes from light seen through doors the point's rays missed, and a defensive mixture against them leaves more noise). Read docs/BUILDLOG.md, `sun-bounce`, before taking it up again.
- [ ] More hardware ray tracing: ray-traced shadows and ReSTIR
  - Ray-traced shadows (exact soft shadows from area lights), ray-traced reflections, then ReSTIR-style sampling of many lights or a path-traced reference mode.
  - **Always optional:** settings in `RenderSettings` (so the debug UI and a player's options menu can reach them) switch each ray-traced feature on or off while the game runs, falling back to shadow maps, reflection probes and the software-traced probes.
  - The game is designed, tuned and tested on the non-ray-traced path first, so it looks right without it
- [ ] Lumen-like global illumination (as in Unreal Engine 5; research, numbers and a staged plan in `docs/LUMEN.md`, read Oct 20 2026)
  - **The idea:** probes placed on the visible surfaces themselves, one every 16 pixels plus more at depth and normal edges, 8x8 octahedral, each tracing its own rays, filtered 3x3 in probe space and over time, so light near a surface is what that surface sees.
  - **What it fixes:** probe parallax. A probe sits a little off the surface and sees what the surface cannot.
    - So beside a lit doorway at night the outside wall glows within a probe spacing of the opening (found Oct 20 2026: the lamp-lit jambs and room seen at grazing angles by the probes in front of the wall; wider on Low, narrower on Ultra), and the back room's ceiling glows beside its doorway.
    - Also indirect shadows smaller than the probe spacing.
  - **In stages:**
    1. prerequisites: 6e's motion vectors and TAA (which average away the probes' jittered placement and directions), a normal target in the depth prepass, a hierarchical depth buffer;
    2. diffuse screen probes with full-length ray queries, hits shaded as the probe rays' are (no surface cache needed with hardware rays), the main pass reading a per-pixel GI image;
    3. importance sampling from last frame's probes and the BRDF;
    4. short-range AO to 16 pixels after the temporal filter;
    5. reflections from the same rays, rough surfaces from the probes.
  - DDGI's cascades stay as the far field (Lumen's world radiance cache) and as the Low setting.
  - Presets probes every 32, 16 and 8 px (Lumen's ladder, each level about half the next's cost).
  - Estimated 1-2 ms on the RTX 5080 at High, to be measured
- [ ] Path-traced mode: every pixel traces random light paths through the scene with several bounces, which gets every shadow, bounce, reflection and soft light right with nothing approximated
  - Real time only with one or two paths a pixel, so it needs ReSTIR's reuse of good samples across pixels and frames and a strong denoiser (and upscaling); high-end GPUs only.
  - Worth it first as a reference: golden views path-traced to convergence offline show what the real-time lighting should look like, so its errors (probe parallax, leaks, missing occlusion) can be measured rather than judged by eye

**Milestone:** path-traced golden views converged offline stand beside the real-time picture, the differences measured; the path-traced mode runs live on a high-end GPU.

---

## Phase 7: Cloth simulation (parked)

The physics half of the dressmaking game: fabric that drapes, folds and piles up without passing through itself, ending with a first dress sewn from two panels hanging on a mannequin. It comes early because it is the other thing we most want to see.

- It builds on phase 5's particle store and collision shapes and phase 6's compute pipelines.
- Cloth code lives in its own module on top of the core, like the FPS and MOBA code.
- The pattern editor, garments on moving characters and the game itself follow in phase 10, once phase 9 has skinned characters to dress.
- The solver's plan and research: `docs/CLOTHGPU.md` (read it first).
- **Parked on Nov 6 2026** (the user's choice) for phases 16 and 17. Its unmerged work stays on the `gpu-self-collision` branch; self-collision is to be rebuilt later with a more real-time friendly system, not finished as it stands.

**Items**

- [x] Job system: a work-stealing thread pool with `parallelFor` (`job-system`)
- [x] Cloth lab: a test ground with a station for each milestone, `--game clothlab` (`cloth-lab`)
- [x] Dynamic meshes: deforming meshes drawn blended between ticks, normals and tangents recomputed (`dynamic-meshes`)
- [x] Particle store: the points as plain arrays, lumped masses, pins, the step (`cloth-particles`)
- [x] XPBD solver: substeps, compliance, stretch constraints, a hanging sheet (`xpbd-cloth`)
- [x] Dragging cloth with the mouse (`cloth-grab`)
- [x] Bending and moving pins (`cloth-bending`)
- [x] Collision with bodies: spheres, capsules and boxes, with friction (`cloth-collision`)
- [x] The solver on every core, bit for bit the one-thread result (`cloth-parallel`)
- [x] A tick budget: an overloaded simulation runs in slow motion (`tick-budget`)
- [x] Isometric bending (Bergou et al. 2006), the hinge kept for creases (`isometric-bending`)
- [x] The yardstick: a 1 m sheet at four resolutions, its stretch, sag and cost (`cloth-yardstick`)
- [x] Cloth on the GPU: the solver (`gpu-cloth-solver`), in the game with the late readback (`gpu-cloth-in-game`), its time in F4 (`cloth-gpu-timing`)
- [x] Vertex block descent with Galerkin coarse levels, on the CPU and the GPU, opt-in (`hierarchical-cloth`, `gpu-block-descent`, `gpu-cloth-levels`, `cloth-point-lanes`); its cost cuts parked in phase 10
- [x] Tethers: a string from each point to its nearest pins, on by default (`cloth-tethers`)
- [x] GPU cloth ticks inside the tick budget, and F4's cloth line saying when a frame had too many passes to time (`gpu-tick-budget`)
- [x] The mannequin: `human.obj` in a cloth lab station with a signed distance field (5 mm samples) the cloth collides with on the CPU and the GPU, a sheet dropped over its shoulders (`mannequin`)
- [x] A dress sewn from two panels on the mannequin, pinned at its shoulders and tethered along each panel, on the CPU and the GPU (`two-panel-dress`)
- [x] The dress-sized test: XPBD fails it held by contact or pinned and tethered, so garments need more (`dress-yardstick`)
- [x] Cheaper substeps: 30 a tick, the fewest edge colours, bending optionally less often (`cheaper-substeps`)
- [x] Self-collision on the CPU: contacts a thickness apart and Planar-DAT's cut, the cloth lab's pile sheet (`cloth-self-collision`)
- [ ] Self-collision on the GPU, parked, to be rebuilt with a more real-time friendly system (what was tried is on `gpu-self-collision`), on by default then: a spatial hash built in parallel, coloured contacts, the cut as an atomic minimum (`docs/CLOTHGPU.md`, The plan, step 5). Its tests: the dress (a crossings column in the dress test), the pile
  - Watch for the swaying hem found in `cheaper-substeps`: the 0.7 cm pinned dress's back hem kept swinging about 6 cm through 20 s, fed by something (not the tethers, nor the sewing). Gone with `dress-ease`'s roomier dress (19 mm in its last second), so it hangs on the drape; suspects if it returns: the skirt passing through itself, friction on the legs
- [ ] Fabric properties: a few presets
  - Different stiffness along the threads (warp and weft) than across them (the bias).
  - Bending stiffness (silk vs denim), mass per area, friction
- [ ] Optional, later: IPC or codimensional IPC (incremental potential contact) as an offline reference for judging garments: guaranteed intersection-free, about a thousand times too slow for real time

**Milestone:**

- a sheet pinned at two corners drapes over a sphere;
- a dress sewn from two panels hangs on the mannequin;
- a sheet dropped onto itself folds into a pile with no intersections (the self-collision stress test);
- a silk sheet and a denim sheet fall visibly differently.

## Phase 8: Active ragdolls

Euphoria-style characters: a simulated body held up by joint motors and kept standing by balance controllers, so a shove makes it stagger, step, fall with its hands out and get up again.

- **The plan:** layers 1-5 of the plan in `docs/RAGDOLLPLAN.md` (read it first: it holds the papers' formulas and numbers and how each layer fits the engine). Layer 6, following animation clips, comes in phase 9 with skeletal animation.
- **Why here:** placed straight after cloth because the rigid-body solver is phase 7's XPBD with rotation and inertia added; quaternions come from phase 6a.
- **Units:** real-world values throughout, as in the papers: gravity 9.81 for ragdolls (the character controller keeps its Overwatch 17.5), SI masses, torques and push forces, so the papers' results can be reproduced.
- **Drawing:** until phase 9 the body is drawn as its capsules, a mannequin.

**Items**

- [ ] Rigid bodies (`rigid-bodies`, moved here from phase 14's rigid-body dynamics)
  - A `RigidBody` component: mass, body-frame inertia, velocity, angular velocity, a compound of capsules.
  - An XPBD substep solver (Müller et al. 2020; 10 substeps per tick, 1.2 kHz), with gyroscopic torque.
  - Capsule contacts against the static level from `contact(capsule, triangle)`, restitution 0 and friction 0.8.
  - Capsules only: there is no oriented box. `Mesh::capsule` to draw them.
  - Shapes behind one interface, so phase 15 can add boxes and convex hulls for destruction
  - Tests: momentum and angular momentum conserved in flight, the tennis-racket theorem's tumble, a dropped capsule rests without jitter, and the same run twice is bit-identical
- [ ] Joints (`joints`): ball and hinge `Joint`s with swing-twist limits and soft limits
  - Capsule-against-capsule self-collision between unjoined parts.
  - The ragdoll builder from `RagdollBodySettings` (1.8 m and 70 kg by default, anthropometric proportions, feet as two parallel capsules for a flat sole).
  - Each part is an entity with a capsule `Collider`, so picking finds ragdolls with no new code
- [ ] Joint drives (`joint-drives`): an inertia-scaled PD servo per joint
  - Stiffness as a natural frequency and damping as a damping ratio (GTA V's units).
  - Applied as equal and opposite torques on the two bodies, capped, and scaled by a global strength and health
- [ ] Gravity compensation (`gravity-compensation`): feed-forward torques from virtual forces (τ = Jᵀ F) that hold the body up, so the drives stay soft
  - Faded out when the centre of mass leaves the feet's support area, so a falling body goes limp
- [ ] Standing balance and stepping (`standing-balance`, `stepping`)
  - The torso held upright in world space with the stance hip taking the balance of the torques (SIMBICON).
  - Foot placement from the inverted-pendulum capture point (Coros 2010), two-bone IK, a step state machine, and failure detection.
  - Headless test: a 300 N push for 0.1 s at the chest leaves it standing 3 s later
- [ ] Behaviours and supervisor (`falling`, `behaviour-supervisor`)
  - Behaviours that score the situation and write targets for the body parts they own (balance, stagger, catch fall, arms windmill, head look, get up), run by a supervisor.
  - Handover between a character's kinematic capsule and its ragdoll and back
- [ ] Debug view and demo: R spawns a ragdoll in front of the camera, E shoves what the view ray hits
  - F4 draws the capsules, joint frames, centre of mass, support area, next foot target and the active behaviours

**Milestone:** shove a capsule mannequin off the obstacle course's steps: it staggers, takes recovery steps, falls with its hands out and gets back up.

## Phase 9: Animation, UI and audio

- [ ] Parent/child entity hierarchy: attach entities to others and to skeleton joints (a weapon to a hand, a hat to a head, a turret to a vehicle)
  - Skeletal animation is the first thing that needs it, and phase 12's first-person weapon uses it
- [ ] Skeletal animation from glTF skins: joints, playing and blending clips
  - Skinning in a compute shader once per frame into a vertex buffer that the main pass, every shadow view, ray tracing and cloth collision all read.
  - Skinning in the vertex shader would repeat the work in every shadow tile
- [ ] Ragdolls follow animation (layer 6 of `docs/RAGDOLLPLAN.md`)
  - A table from skeleton bones to ragdoll bodies, clip poses as the joint drives' targets (a powered ragdoll).
  - Skinned characters drawn from the bodies, softening and shaped impulses on a hit.
  - Then a smooth blend back into a matching clip or get-up animation
- [ ] Text rendering with a bitmap font (debug overlays already have a stroke font, `debugText`, good for numbers but not for real UI)
- [ ] HUD: crosshair, health bars above units, ability cooldowns
  - Start with flat-coloured screen-space shapes drawn after phase 6's tone mapping (no depth test, alpha blending) and a tested `worldToScreen(camera, point, windowSize)`.
  - Turn the demo's MOBA destination marker (an emissive plane for now) into a HUD ring of constant pixel size
- [ ] In-game stats in the debug UI (phase 6a): entity count, and phase 5's frame, tick and per-system timings
- [ ] Key bindings: games read actions (`MoveForward`, `LockCamera`) instead of keys, and players can rebind them
- [ ] Audio with SDL3: play sounds, volume, simple 3D panning by distance and direction

**Milestone:** an animated character that walks and idles with something held in its hand, with a HUD, sounds and a debug overlay.

## Phase 10: Garments and a dressmaking game

A third kind of game, on top of phase 7's cloth: cut fabric panels, sew them into garments and see them hang and move on a body. Needs phase 9's skinned characters and phase 6's physically based materials.

**Garments**
- [ ] Garments from any panels: phase 7's two-panel dress (outlines cut into flat meshes, seams pulled shut) for the pattern editor's panels and seams, with darts, gathers and sleeves
- [ ] Tethers for worn garments: anchors on a moving body (a shoulder seam, a waistband), each point tied to the nearest point of each region, with the distance along the sewn pattern across seams (exact geodesics, Xin & Wang 2009); within one flat piece, round its armholes, phase 7's dress already measures it (`ClothRestPaths`)
- [ ] If garments held by contact stretch (phase 7's dress decides): vertex block descent's coarse levels made cheap enough: the two finest levels skipped, the line search's reduction in one pass, the move folded into the next level's gradient, a workgroup per coarse point for its gather. Still to try: Stable Neo-Hookean membrane, the adaptive initial guess, Chebyshev, affine coarse handles (Xian 2019)
- [ ] A coarse simulation mesh driving a fine render mesh: the cloth solved on a few thousand points, each drawn vertex following its nearest simulated triangle (what shipped games do; 1,500 points instead of 10,000 is about 7 times less work, barely visible)
- [ ] Dressing an animated character: the cloth collides with the skinned body as it moves (its distance field following the bones, the surface's velocity in the friction)

**Rendering**
- [ ] Weave detail with phase 6a's normal maps
- [ ] Fabric shading: a sheen term for the soft rim highlight of cloth (glTF's KHR_materials_sheen), on top of physically based materials (moved here from phase 16)

**Game**
- [ ] Pattern editor: draw and edit panel outlines, choose which edges are sewn together
- [ ] Fabric choice, colours and prints per panel

**Milestone:** a garment drawn in the pattern editor hangs on the mannequin and swings as it turns; then it follows an animated character as it walks.

## Phase 11: Networking

Builds on the fixed tick and the headless simulation. Commands go up, snapshots come down.

Deferred until there is a playable FPS slice (phase 12) worth playing together. The deterministic, headless simulation keeps it possible, and phase 6a's determinism test keeps it that way.

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
- [ ] Asset cooking (before large scenes): an offline step in `tools/` that turns source models and images into ready-to-load files
  - Texture compression (BC7 and BC5) moved to phase 18 on Nov 13 2026; cooking would then save its results instead of compressing at load.
  - Meshes in vertex-cache order (meshoptimizer); their levels of detail come with phase 18's item.
  - Tangents precomputed
- Level of detail moved to phase 18 on Nov 13 2026 (instancing itself moved to phase 6c)
- [ ] Cloth level of detail and sleeping, for many characters' capes and skirts: distant cloth runs fewer substeps or keeps its last pose, and cloth at rest stops being simulated until something touches or moves it

**FPS**
- [ ] Hitscan and projectile weapons, recoil and spread
  - Projectiles and grenades (phase 5's particles) must also hit moving things, characters and props, not only the static level.
  - Sweep the sphere against the moving colliders' shapes too (a sphere or capsule is a raycast against the shape grown by the radius, like `sweepSphere`; a box needs a rounded-box sweep)
- [ ] Health, damage, death and respawn
- [ ] Decals: bullet holes, scorch marks, signs and dirt projected onto surfaces, listed per screen tile as the lights are (moved here from phase 16)
- [ ] First-person weapon model (attached with phase 9's hierarchy) and camera feel (bob, field of view)

**MOBA**
- [ ] Pathfinding: A* on a grid first, then a navigation mesh
- [ ] Unit selection and attack-move commands
- [ ] Abilities with cooldowns, mana and targeting (point, unit, area)
- [ ] Minion waves with simple AI, towers
- [ ] Fog of war

**Milestone:** a small online deathmatch, and a 1v1 lane with minions and towers.

## Phase 13: Orbital simulation (a solar system)

A fourth kind of world: the Sun, planets and moons at real scale, moving under gravity. Like cloth, it lives in its own module on top of the core.

It needs phase 5's particles and release build but no collision, animation or networking, so it can move earlier whenever we want it sooner.

**Precision and rendering**
- [ ] Double-precision vectors (`DVec3`) for simulation positions
  - A float keeps about 7 significant digits, so at Neptune's distance (4.5 billion km) positions would snap in steps of hundreds of kilometres; a double's 16 digits give about a millimetre.
  - Float `Vec3` stays for rendering and everything else; phase 5's per-particle functions (`fly`, `bounceVelocity`) become templates over the vector type
- [ ] Camera-relative rendering in double (floating origin)
  - Phase 6a makes rendering camera-relative in float; here the subtraction happens in double before the small difference becomes a float.
  - So the GPU only ever sees numbers near zero and far-away objects do not jitter
- [ ] Depth at planetary scale: phase 6a's reversed-Z, plus a logarithmic depth or split near and far passes if a nearby spacecraft and a planet millions of kilometres behind it still do not both sort correctly

**Simulation**
- [ ] N-body gravity with a symplectic integrator (leapfrog / velocity Verlet)
  - Ordinary integrators slowly pump energy into or out of an orbit, so planets spiral outwards or fall into the Sun; symplectic ones keep the energy error bounded forever.
  - Tested: total energy stays within a small band over many orbits
- [ ] Its own simulation clock with time warp: many substeps per 120 Hz tick so a year can pass in seconds
  - The step size adapts to how close bodies come to each other.
  - A naively varied step breaks the symplectic guarantee and the energy drifts again, so the adaptivity has to keep it (for example a time-symmetric step choice, or fixed-step substepping only for the bodies in a close pass)
- [ ] "On rails" Keplerian orbits for distant bodies: the position comes from the exact ellipse at any time, with no integration error and almost no cost
  - A body switches to full simulation when the player comes near (and back when they leave), so a spacecraft's thrust or a close pass can change its path
- [ ] Barnes-Hut (or the fast multipole method) for thousands of bodies, such as an asteroid belt
  - A distant cluster pulls like a single mass at its centre, as a far galaxy does, turning the O(n²) pair sum into O(n log n).
  - Drawn with phase 6c's instancing

**Milestone:** the Sun, planets and Moon orbit stably for 100 simulated years at high time warp, and you can fly the free camera from Earth to Neptune without jitter.

## Phase 14: Later and optional

Ideas with no fixed place and nothing waiting on them: pick one up when a game needs it.

### Physics

- [ ] More rigid-body dynamics, extending phase 8's solver (bodies, joints and capsule contacts come with the ragdolls)
  - Oriented boxes with box contacts (separating axes), stacking and piles of crates (or with phase 15's convex bodies, whichever comes first).
  - **Particles colliding with each other** (grenades knocking each other about) are part of this. They were left out of phase 5 because piles need this phase's iterative contact solver:
    - find pairs with the uniform grid over each particle's swept box;
    - detect the touch by sweeping in relative motion (a ray against a sphere of the summed radii, so fast crossings are not missed);
    - respond by running the relative velocity through `bounceVelocity` and sharing the change by mass (an equal and opposite impulse, so momentum is conserved), with the two materials' restitution and friction averaged
- [ ] A lathe (`lathe`): a spinning part turned smaller by a tool, the material cut off flying away as chips (the user's idea, Oct 30 2026)
  - The part kept as its profile r(z), a surface of revolution, as CNC lathe simulators keep it: the tool sets r(z) = min(r(z), its own profile), and the mesh is rebuilt by revolving the profile into rings (normals from dr/dz).
  - Chips carry the removed volume, pi (r_old^2 - r_new^2) dz a sample, times the density, leaving along the tangent at the surface speed omega r: fragments for brittle materials (particles, then phase 15's convex bodies), later a curling ribbon for ductile metals (a stiff, curled strip on the cloth solver).
  - Optional: the cutting force F = k_c x depth of cut x feed a turn (k_c about 1500-2000 N/mm^2 for steel, **from memory**: check it before writing tests), its power F v slowing the spindle.
  - The general follow-up, for parts that do not spin, milling and carving: the tool subtracted from a signed distance field, d = max(d_part, -d_tool), remeshed by surface nets.
  - Tests: a profile cut to a set radius revolves into a closed mesh of that radius, the chips' mass equals the removed volume's, a tool clear of the part removes nothing

### Lighting and shadows

- [ ] A software tracer for GPUs without ray tracing, as the lower quality setting
  - **What:** a global signed distance field round the camera: clipmaps of sparse 8^3 bricks holding a narrow band of distances, built in compute from the scene's triangles with an eikonal fill, the static level cached apart from what moves.
  - **Tracing:** sphere marching in place of the probe rays' ray queries, its hits lit from a coarse volume of the scene's albedo and emissive times the probes' light, checked against the ray-traced result.
  - Probes only need blurry diffuse light, so coarse geometry is enough.
  - **Why not voxels:** chosen over a voxel grid (Oct 20 2026). Epic tried voxels for Lumen (cone tracing, then bit bricks) and dropped them as leaky and slow for exactly this, and AMD's Brixelizer builds the same at run time.
  - Lumen's numbers and robustness fixes (walls thinner than a voxel, the half-voxel expansion) in `docs/LUMEN.md`
  - **Moved here from 6d** (Oct 23 2026, the user's choice): a large piece of work that only GPUs without ray tracing would use; image quality (6e) comes first.
  - **On hold with DDGI** (Nov 6 2026, the user's choice): it feeds the probes, so phase 16 uses screen-space GI instead. It is the way to fix closed rooms on these GPUs if that is ever wanted.
  - **Expected, not measured:** close to the ray-traced look, since the probes' light is blurry anyway; leaks through walls thinner than the field's voxels unless thickened, thin objects (the light lab's fence) lost at the coarse levels, coarser colour bleeding at hits, and moving objects stamped in each frame. Perhaps 0.5-2 ms for the default's rays on a GTX 1060-class GPU; slower than ray queries on an RTX card.
- [ ] Physical light units and a camera exposure for all lights
  - Emissive surfaces and the lights they stand for could then be linked physically (the light a surface receives from the moon is the moon's brightness times the solid angle it covers).
  - The exposure would map the real dynamic range (the moon disc is 10,000+ times brighter than moonlit ground) to the screen.
  - The night's look waits for it too: night vision's thresholds (now off by default), lower contrast at night (rods see less of it; the ground under a dim moon is crushed in the tone curve's toe while the sky shows), and the sky round a low or crescent moon (partly the sun's twilight) against the dark ground.
  - Until then the demo's moon glow and moonlight, and the light lab's sun and sky, are separate artistic dials
- [ ] The sky visibility grid's lamps (phase 16's stage 3, optional): each point keeping its brightest few lamps found by shadow rays, refreshed in a region when one switches, so lamps bounce light without probes
- [ ] Anisotropic highlights (brushed metal, hair, satin): GGX stretched along the tangent, glTF's KHR_materials_anisotropy
- [ ] Subsurface scattering for skin, when the engine has characters with skin: light entering and leaving the skin a little way off, softening it (a screen-space blur by a diffusion profile, or a pre-integrated skin lookup)
- [ ] Tighter clusters: depth bounds per tile from the prepass and smaller far tiles, since far clusters list 16 or more lamps where 4-8 reach; only if many lights ever cost more than a distance check each
- [ ] Shadow redraw budget: at most N shadow tiles drawn per frame, most important first, far lights refreshed less often, so switching on many lights at once or turning quickly does not cost one long frame
- [ ] Separate shadow caches for still and moving casters: keep a copy of each tile with only the still world, and draw the moving things over a copy of it each frame, so one spinning object no longer forces whole tiles to redraw
- [ ] Virtual shadow maps (as in Unreal Engine 5) for the sun, in place of cascades or beside them as the top setting
  - **How:** one huge virtual shadow map (16384 x 16384 per clipmap level, nested levels each twice the last's size around the camera) cut into 128 x 128 pages. Only the pages that pixels on screen actually look up are drawn, into a pool of physical pages, and kept between frames until something in them moves.
  - **Gain:** about one shadow texel per screen pixel at every distance, with no cascade seams, and caching makes still scenes nearly free.
  - **Needs:** a pass marking the pages the depth buffer touches, a page table the shaders read through, GPU-driven culling and drawing per page, and invalidation from moving casters.
  - A large project; after DDGI (6d) and TAA (6e), and only if ray-traced sun shadows turn out too slow on typical GPUs

### Performance and scale

- [ ] Reuse per-frame memory
  - The frame's draw list, the shadow tile lists and the debug lines are rebuilt in fresh vectors every frame; keep and clear them instead.
  - The demo rebuilds its `CollisionScene` on every `pick` (hover and a click can both pick in one frame); rebuild it once per frame.
  - Minor each, so only if phase 6b's timings point at them (the renderer timing breakdown, cheaper caster culling and instancing that used to be listed here moved to phases 6b and 6c)
- [ ] GPU-driven rendering beyond phase 6c
  - Hi-Z occlusion culling moved to phase 18 on Nov 13 2026; still here: writing indirect draws with a GPU-written count (bounds go into the GPU instance records then).
  - Meshlets (meshoptimizer) and mesh shaders where the GPU has them.
  - A visibility buffer if scenes ever get dense enough to need it
- [ ] Open-world scale: streaming textures and meshes in and out by distance (impostors moved to phase 18 on Nov 13 2026), and a render graph that orders passes and places barriers from what each pass reads and writes, once the hand-placed barriers of phase 6b no longer keep up
- [ ] The sky grid denoised: each point blended with its neighbours that see it (`skyCornerSees`, so never through a wall), weighted by how sure each is (its escaped rays), as bakers denoise lightmaps (added Nov 12 2026, after the deep rays near openings)
- [ ] The sky grid's rays aimed at openings: doors and windows found automatically (rays that escaped, clustered by direction, or the level's gaps between rooms), more rays sent through them and weighted back (importance sampling, as bakers' portals), with no hand-placed volumes (added Nov 12 2026)
- [ ] Sky visibility baked offline and streamed, for big worlds (added Nov 11 2026)
  - The grid only bakes against the static level, which does not change, so a level can be baked once, automatically (no hand-placed volumes), saved, and streamed in tiles by distance with the world, as open-world games do with their probe grids and sky occlusion.
  - Bake geometry, not light: what is baked (how much sky each place sees) is combined with the sun and sky at run time, so the time of day stays free.
  - Sparse, adaptive storage, following surfaces rather than volume: fine points (0.5-1 m) near walls, streets and interiors, coarse ones (several metres) over open ground and air, none inside solid buildings (bricks allocated only near geometry, or an octree subdividing only where the value changes; a uniform 0.5 m grid over the light lab's 800 x 800 m would be about 2.4 GB). Coarse tiles loaded first, read ahead along the camera's velocity.
  - The runtime store (`SkyCellStore`, `ao-temporal`) is its first half: points kept by their place in the world; this writes that store to disk.
  - The runtime bake stays for what changes (phase 15's falling buildings, re-baked locally by the background bake).
  - Offline, it can afford thousands of rays a point (bakers use about 512 to 4,000 a texel), and the two cures below.
  - Agreed Nov 11 2026 (option C: the background bake first, `ao-bounce-unfog`, then this as its own project): what it stores is geometry and albedo (the sky's reach and the bounce per unit of sky light), so it is baked once per level; the sun's bounce (phase 16 stage 2) depends on the sun's place and stays a runtime layer on top. Plan it with a research write-up first (formats, sparse layouts, streaming, what shipped engines store), as for the other big systems.

### Tools and the rest

- [ ] Slang trial: one shader source compiled to both SPIR-V and C++, which could replace the hand-written CPU mirrors of the shader libraries
- [ ] Particle effects for visuals (sparks, smoke, ability effects)
- [x] Commands from the command line: every setting, pause and step, the camera, screenshots and stats, sent to a running game or run as a script (`command-line-control`)
- [ ] Level editor or scene file format
  - Every editing operation (placing, moving and deleting things, saving a level) a command in the `CommandRegistry` (`core/command_registry`), the editor's buttons issuing them, so a script or an AI can do whatever its windows can, and undo is built on the same list
- [ ] API reference with Doxygen: a `make docs` target generating HTML into `build/docs/`, with Graphviz include and dependency diagrams (they show the source layering)
  - Needs function comments switched from `/* */` to `/** */` so Doxygen picks them up.
  - Complements `docs/ENGINE_GUIDE.md`, which explains concepts rather than the API
- [ ] Scripting for gameplay code

## Phase 15: Destruction and a flying superhero

A fifth kind of game (the user's idea, Oct 27 2026): fly fast enough to feel it, and bring down whole buildings that fall by real physics. Buildings are chunks bonded together, carrying their weight down to the ground; break the right supports and the load shifts, the next supports overload, and it comes down floor by floor.

- **The plan and research:** `docs/DESTRUCTION.md` (read it first): the feel of speed, rigid bodies for debris, fracture, why buildings collapse (bond stress, progressive collapse, real materials' numbers), dust and light.
- **When:** numbered after phase 14 to keep the other phases' numbers; it can start any time after phase 8's rigid bodies, and is better after phase 9's audio (sound is half of feeling fast).
- **Units:** real-world values, as for cloth and ragdolls: gravity 9.81, real sizes and material strengths (concrete 20-40 MPa in compression), so big things fall as slowly as they should.

**Items**

- [ ] Convex rigid bodies (`convex-bodies`): boxes, then convex hulls, on phase 8's XPBD bodies
  - Contacts by separating axes (boxes) and GJK with EPA (hulls), friction, sleeping and islands.
  - Speculative contacts, so fast debris does not pass through floors.
  - Tests: a box resting on a slope at its friction angle, a stack standing still, momentum through collisions, bit-identical runs
- [ ] Fracture (`voronoi-fracture`): each building's blocks cut into Voronoi chunks at load, more seeds where hits are likely
  - The bond graph: chunks sharing a face are bonded (its area and normal); chunks on the ground are supports; intact parts clustered into one body (as Unreal's Chaos does).
  - Tests: the cells fill the block (volumes sum), each is convex, bonds only across shared faces
- [ ] Blade cuts (`blade-cuts`): aim a plane in slow motion and whatever it crosses splits along it, the halves falling as rigid bodies (the user's idea, Oct 30 2026, after Metal Gear Rising's Blade Mode)
  - `clipMeshByPlane`, shared with `voronoi-fracture`: vertices sorted by their signed distance d to the plane, each crossing edge split at t = d_a / (d_a - d_b) with its positions, UVs and normals blended, the cut's loops capped (ear clipping, since a cap need not be convex) with an inside material and UVs projected onto the plane.
  - Each half's mass, centre of mass and inertia from its closed surface (the divergence theorem), its collision one convex hull or several; new meshes uploaded and given acceleration structures as they appear, the pieces capped in number and the tiny ones removed.
  - Slow motion by scaling the time fed to the simulation, the aiming at full speed; a command cuts along a given plane.
  - Tests: the halves' volumes sum to the whole's, each half is closed (every edge shared by two triangles), a box's halves have their hand-calculated centres of mass and inertia, a plane missing the mesh leaves it whole
- [ ] The stress solver (`stress-solver`): statics on the bond graph, K u = f by conjugate gradients, warm-started
  - Each bond's force split into compression or tension along its normal and shear across it, as stress over its area (as NVIDIA Blast does).
  - An intact building is kinematic and costs nothing until something changes it; impacts are forces over their duration.
  - Tests against hand calculations: a column's base carries the weight above, a cantilever's root the bending moment w L^2 / 2, a beam on two supports splits its load by the lever rule, a removed support's load moves to the others
- [ ] Breaking (`bond-breaking`): elastic and fatal limits (damage between them), pieces cut off from the ground found by union-find and turned into falling clusters, the excess force into the pieces' speed
  - Tests: a ten-storey frame stands with a quarter of its base columns gone and collapses with half (the worked example in `docs/DESTRUCTION.md`)
- [ ] On the GPU (`gpu-destruction`): thousands of chunks and the stress solve as passes, measured on a ladder of building sizes
- [ ] Debris and dust: small debris as particles that hit the world but not each other; dust as density in the volumetric fog's grid, lit for free; each collapse a `ProbeEvent`
- [ ] Flight (`flight-controller`): hover, cruise, boost and a quick stop, turns limited by sideways acceleration so speed widens the arcs, every move a sweep
  - The feel: field of view widening with speed, camera shake, motion blur from TAA's velocity image, wind streaks, a vapour cone and sonic boom near Mach 1, hit-stop on big impacts.
- [ ] A destruction lab (`games/destructionlab/`): a few buildings of different structures (a concrete frame, a brick house, a steel tower) to fly at and judge
- [ ] A city, once the lab's buildings work: streaming, levels of detail and a floating origin (phase 12's, phase 13's and phase 14's items)

**Milestone:** fly into the base of a ten-storey concrete building at the speed of sound; its load shifts, the next columns fail, and it comes down floor by floor in about three seconds, in a cloud of dust.
