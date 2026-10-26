# GameEngine

A game engine written from scratch in C++20, as a learning project.

The long-term goal is an engine that can run both an **FPS** and a **MOBA**, and eventually a dressmaking game with real cloth physics. The core stays genre-agnostic, and genre-specific code (camera styles, weapons, abilities, netcode) sits on top.

It began life as a software rasterizer drawing pixels on the CPU, and now renders on the GPU with its own Vulkan 1.3 renderer.

**Further reading:**

- [ROADMAP.md](ROADMAP.md): what has been built and what comes next
- [docs/BUILDLOG.md](docs/BUILDLOG.md): how each step went
- [docs/ENGINE_GUIDE.md](docs/ENGINE_GUIDE.md): how it all works

## Features

### The renderer

- **Its own Vulkan 1.3 renderer**, built on volk, vk-bootstrap and VMA, with dynamic rendering, synchronization2 and hand-written barriers. It replaced SDL_GPU, drawing every golden image bit for bit the same.
- **Frames in flight:** two by default, so the CPU records the next frame while the GPU draws this one, or one for the least latency. Switchable while running.
- **A GPU scene:** every object, material and light sits in storage buffers written once per frame, and every texture in one bindless array. Each view draws one instanced draw per mesh, so the lamp field's 161 visible objects take 10 draw calls. Shadow casters are first culled against each light's range sphere.
- **A depth prepass:** the scene's depth is drawn first, so each pixel is shaded once, for the nearest surface only, whatever order objects are drawn in. Switchable in the debug UI. Lights behind a surface are skipped before their shadow lookups.
- **Clustered shading:** a compute pass splits the view into a grid of clusters and lists, as one bit per light, the lights that can reach each. A pixel loops only over its own cluster's lights, so up to 1024 point lights and 256 spotlights cost only where they shine.
  - The picture is identical to the last bit with it on and off.
  - A heatmap debug view shows how many lights each cluster lists.
- **Hardware ray tracing** on GPUs that have it (ray queries: any shader can ask what a ray hits first).
  - An acceleration structure per mesh, compacted, and one rebuilt from every object each frame.
  - A debug view traces one ray per pixel and shows normals, albedo, or front and back faces.
  - The foundation for global illumination. It is optional, so GPUs without it simply leave it off.
- **Reflections:** glossy surfaces mirror their surroundings, blurrier the rougher they are, from two sources.
  - Screen-space reflections: each pixel follows its mirror ray through a pyramid of the depth image and reads last frame's picture where it hits.
  - A reflection probe round the camera for what the screen does not show (behind you, off its edges): a 360-degree picture captured every frame, with each surface's distance, so reflections land in the right place wherever the shiny surface is, and pre-blurred copies for rougher surfaces. On GPUs with ray tracing the sun's shadows in it are traced where the shadow cascades do not reach, and a setting (Ray-traced, on by default: the Ultra preset's) traces each mirror ray instead, so what a mirror shows is exact even where the probe cannot see it.
  - Mirrored lights, on GPUs with ray tracing (Mirrored lights in F5 Reflections, on by default: the Ultra preset's): a flashlight shone into a mirror lights the spot its reflection points at, the mirror's shape cut out of the beam, and a chrome ball spreads it faintly all round. Each pixel finds the point on the shiny surface where the law of reflection sends the light to it (manifold next event estimation: Newton's method on the surface, with rays), and the surface's curvature sets how bright it is. Brushed metal softens the spot's edges (its light gathered from a patch round that point), the lit spot bounces round the room through the light probes, and reflections show it.
  - Where neither finds anything, and on rough surfaces, the light probes stand in. Settings in F5 (Reflections), with a confidence debug view; off on the Low preset. The light lab has a polished slab and a mirror ball north-east of the start, and a mirror in the sunlit room facing the doorway with a brushed steel panel beside it (a rough mirror), to try them on.
- **Ambient occlusion (GTAO):** creases, corners and the gaps under things darken in the light from all around, where the light probes are too far apart to see them. Each pixel finds, along a few lines through it on screen, how high the surfaces round it rise within half a metre (from the depth image), and the share of the sky between them comes from a closed-form integral. A crease between bright walls darkens less (light bounces back in), and reflections dim in creases too. Settings in F5 (Ambient occlusion); off on the Low preset.
- **Volumetric fog:** light scattered by the air, so the sun's beams through a window, a flashlight's cone and the street lamps' halos show in the mist, and distant things fade into it. A game places a `Fog` (how thick it is at the ground and how fast it thins upwards, its colour and how much it scatters forward); the light lab has a thin ground mist (its settings under Mist in F5). The air in view is cut into a grid of small cells (froxels) that follows the camera, each lit by the sun with its shadows, the lamps its part of the screen lists with theirs, and the light probes' light from all around; then each pixel is dimmed and lit by the fog in front of it, glass too, and past the grid's 100 m the fog's closed form takes over. Settings in F5 (Fog); off on the Low preset.
- **Temporal anti-aliasing (TAA):** each frame the picture moves by a different fraction of a pixel, and each pixel averages about the last ten frames, so edges are smooth and thin things stop shimmering.
  - Each pixel finds where its surface was last frame: from its depth and the two cameras, or from a motion vector a small pass draws for anything that moved.
  - History that no longer fits is clipped into the range of colours its neighbours show now, so moving things leave little ghosting.
  - One anti-aliasing choice in the settings (Upscaling): Off, TAA, FSR 3.1 or DLSS (the default); FSR and DLSS replace TAA rather than running with it. TAA has its own tuning and a motion debug view (TAA).
  - It also upscales: a render scale (Upscaling, 1 by default, down to 0.33, or an upscaling mode at the top of the settings: Native, Quality, Balanced, Performance, Ultra performance) draws the scene smaller, and TAA builds the full-size picture from the jittered frames, for speed on weaker GPUs.
  - Or AMD's **FSR 3.1** upscales instead (Upscaling, Upscaler), on any GPU, with its sharpening. It ships as AMD's signed DLL beside the game; without it, TAA upscales.
  - Or NVIDIA's **DLSS** (DLAA at Native), on RTX GPUs, through NVIDIA Streamline. Its DLLs are not in the repository (NVIDIA's licence): `python tools/fetch_streamline.py` downloads them. Without them, TAA upscales.
- **Reversed-Z depth:** near at 1 and far at 0, so depth precision is nearly even out to the far plane.
- **Camera-relative rendering:** positions are measured from the camera before they become floats in matrices, so distant scenes do not jitter.
- Indexed meshes, and textures with mipmaps and trilinear plus anisotropic filtering.

### Global illumination: light that bounces (DDGI)

**Where the light probes live**

- Light probes in nested grids that follow the camera: six by default, reaching about 116 m. The densest are half a metre apart, and each further grid twice as far apart.
- Each grid is a circular buffer, so moving only brings in a plane of new probes, which start from the coarser grids' light.
- Nothing to place per level. On in both the light lab and the demo.

**What each probe gathers**

- 256 rays a frame by default (set by the quality preset: Low 32, Medium 64, High and Ultra 256; up to 1024 by hand). Rays are stored only for the probes traced that frame, so even 1024 rays a probe take about 200 MB. Over 16 frames the rays take turns through one long spiral, so between them they cover every direction evenly: as calm as three times the rays.
- Rays bring back the sky, and surfaces lit by the sun and the brightest nearby lamps, found by shadow rays.
- They also bring back the light the probes held last frame, so each frame adds a bounce.
- What comes back is blended into small octahedral images: the light arriving from every direction, and the distance to the nearest surface.
- The blend is smoothed over time by time constants in seconds, so it behaves the same at any frame rate.

**Keeping it calm and quick**

- **Ray budget** (optional): a number of probe updates per second, above which each frame traces a share of the rays, so the probe work stops growing with the frame rate.
- **Adaptive memory** (optional): each texel measures its own noise, keeps a long memory while its light only wobbles within it, and starts afresh when the light really changes. A steady room is calmer, and a lamp switched on still comes in within a second.
- **Scene events:** a game tells the probes about a change it knows of (a lamp switched, the clock moved). The probes round it check where the light really changed and start afresh only there: quickly where the change is big, not at all where it hides in the noise. A lamp's bounce comes and goes within a fraction of a second, while dim corners it barely touches stay calm.
- **Sharing** (optional): neighbouring probes each trace their own directions, so their errors are independent, and read each other's light. It halves the flicker in a dark room at 30 fps, for slightly softer bounce light.
- **Fast lights** (a flashlight, headlights) keep their bounce in a second set of images with a memory of a twentieth of a second, and the probes near one trace as many rays as the rest (256 by default) every frame. A swung flashlight's bounce follows it instead of trailing behind for seconds. A setting turns this off, leaving fast lights to the main memory, for use with the adaptive memory.

**Lighting the surfaces**

- Every pixel blends its 8 nearest probes, weighted by nearness and by whether each probe can see it at all (a statistical test on its stored distances).
- So sunlight through a window lights the whole room, warmly where it bounces off the floor, without leaking through walls into the next.

**Probes that move and sleep**

- Probes that land inside walls move out, or switch off when they cannot. Probes a hair from a surface move away from it.
- Only probes some surface reads are traced: those beside the static level, and those something moving comes near. In the light lab's dense volume this made the probe work four times cheaper.
- A debug view draws a ball at each probe, showing the light it holds or its state (off, asleep, woken by something moving).

### Lights, materials and shadows

- **Lights as components:** directional, point, spot and ambient. Up to 1024 point lights and 256 spotlights at once.
- **Physically based materials** (glTF metallic-roughness): GGX microfacet highlights with Smith shadowing and Schlick Fresnel, Lambert diffuse, and energy compensation for light that bounces between microfacets, so nothing is lost or created at any roughness.
- **Emissive** (glowing) materials, with glTF's emissive textures for patterns (screens, dials, labels).
- **Baked occlusion:** glTF's occlusion textures darken the light from all around in a model's creases, combined with the screen's ambient occlusion (the more closed of the two counts).
- **Cut-outs and double-sided materials** (glTF's alpha mask and `doubleSided`): leaves, fences and grates with holes, in shadows and ray tracing too, their mips built to keep their coverage so fences do not vanish with distance.
- **Glass and blended surfaces** (glTF's alpha blend and `KHR_materials_transmission`): sorted far to near and blended over the scene, glass reflecting by Fresnel and tinting what is behind it per channel, with a reactive mask so TAA, FSR and DLSS do not smear it; on ray-tracing GPUs light passing through glass takes its colour (shadows, bounce light and reflections). Thick glass (`KHR_materials_volume`, `KHR_materials_ior`) bends what is seen through it and frosted glass blurs it, more the further behind; the light lab has a glass ball and a frosted panel beside its chrome ball.
- **Frustum culling:** each view, camera or shadow, skips what it cannot see.

**Shadows from every light**

- Point lights as six cube faces, minus the faces that cannot shadow anything on screen. Spotlights as one view each.
- The sun as four cascades: box views from sharp near the camera to coarse out to 200 m.
  - Or only as far as the farthest surface on screen, measured on the GPU each frame, so in a room every texel is 7 to 17 times finer.
  - Sized so turning never makes them shimmer, and blended at their seams.
- All in one depth atlas, with tiles sized by each light's reach, so up to 64 point lights and 8 spotlights cast shadows.
- Soft edges that widen with distance from the blocker, like real ones (PCSS), sized by each light's glowing size, and for the sun by its half-degree disc.
- Cached: a tile is only redrawn when its light, or something in its view, moves.

**Light priority**

- With more lights than the shader and shadow atlas hold, the brightest ones where the action is win, and lights out of view are skipped.
- Hysteresis keeps shadows from flickering between similar lights.
- A game can set a priority, so a light is never dropped while in view.

### The sky

- **A physically based sky and atmosphere** (Hillaire 2020). Light through the air is dimmed by Beer-Lambert's law and scattered by molecules (blue far more than red) and aerosols (mostly forward). Light scattered many times is summed as a geometric series. All of it is kept in three small tables computed on the GPU.
- The sky is blue overhead and pale at the horizon, and the sun reddens and dims as it sets, its disc glowing through the air.
- Probe rays that escape bring back the real sky's light, and open ground gets the sky's light.

### Colour and the image

- **Gamma-correct (linear-light) colour**, with sRGB textures and swapchain.
- **HDR rendering:** the scene is lit in 16-bit floats, then tone mapped with an exposure and a choice of curves (AgX, ACES, Reinhard or plain clamping), so bright lights roll off smoothly instead of clipping to white.
- **Bloom:** a few percent of all light scattered into a soft halo, as in a camera lens. The scene is halved down a chain of levels with Jimenez's 13-tap filter and grown back with tent filters, energy-conserving, with a firefly filter.
- **HDR output** on HDR monitors: scRGB, with paper white and peak brightness settings in nits, falling back to SDR when Windows HDR is off.
- **Eye adaptation** between day and night, and, off by default, night vision: a dim night seen in the rods' blue-grey.
- Dithering against banding.

### Models and assets

- **Model loading:** OBJ, and glTF/GLB with embedded or external textures.
  - Metallic and roughness, as factors and textures.
  - Normal maps, with the file's tangents or tangents generated with MikkTSpace as baking tools do, so the bumps look as their authors made them, mirrored pictures included.
  - Normal map filtering (Toksvig): bumps too small to see far away become extra roughness, so distant bumpy surfaces do not sparkle.
  - Emissive colour, with KHR_materials_emissive_strength.
  - Optional smooth normals with a crease angle, for models that have none.
- **An asset manager** that loads each file once and hands out typed handles.
- **Cloth** (being built): a sheet's points held by stretch constraints, solved with XPBD in 15 substeps a tick, with stiffness from real fabric (1 / compliance, about 2000 N/m for cotton), mass from its weight per area, bending stiffness from the fabric's measured rigidity (folds sized as real cotton's; isometric bending, a fixed weighted sum of each hinge's points, about four times cheaper than the angle between triangles, which stays as an option for pressed creases), pins the game can move (hooks on a rail), and air drag. It collides with every collider shape (spheres, capsules, boxes) with friction, so it drapes over a ball, and with bodies of any shape given as distance fields (below). Cloths are stepped a job each, and a big cloth's constraints are coloured so they are solved on every core, with the same result to the last bit as on one. A hand can grab any point and pull it, a stiff spring that lets the cloth be flung.
- **Distance fields for bodies of any shape** (the cloth lab's mannequin): a closed mesh turned once, at load, into a grid of signed distances, how far each grid point is from the skin, negative inside, in a band a few centimetres round it. Between grid points the eight around are blended, and the blend's slope points out of the body, so cloth asks a human figure the same two questions it asks a ball: how far, and which way out. The mannequin's 5 mm grid is 6 million samples (25 MB), built in under half a second on every core, and read the same way on the GPU.
- **Tethers against stretch** (F5's Cloth group: Tethers, Tether slack; on by default): an invisible string from each point of a cloth to each of its nearest four pins, as long as the cloth between them at rest plus a little slack (0.2%), pulling the point back only when taut. The solver passes a pin's pull along about one edge a pass, so a fine sheet hangs stretched (10% at 100 squares a side); the strings reach the pin at once, so a hanging sheet stretches about 0.2% at any resolution, as real cotton does, for about a fifth more than XPBD alone on the CPU and no extra pass on the GPU. They never stop the cloth folding or swinging, and cloth with nothing pinned gets nothing from them.
- **Sewing** (the cloth lab's dress): garments cut from flat pieces, as a dressmaker's paper pattern draws them. Each piece's outline is filled with near-equilateral triangles about a centimetre apart (a triangular lattice inside, the outline's points along its edges, joined by a Delaunay triangulation), so the fabric is equally stiff every way. The pieces are laid round a body, and their seams pull shut at a set speed (Seam speed, F5's Cloth group, 1 m/s) until each pair of sewn points is one point, a rigid weld; the fabric bends across a seam as it does elsewhere. Tethers on a sewn piece are measured along the fabric, round its armholes, never across the cut-outs, and never cross a seam.
- **A second cloth solver with coarse levels against stretch** (F5's Cloth group: Solver "Vertex block descent", then Coarse levels; XPBD by default): vertex block descent (Chen et al. 2024) treats each substep as an energy to minimise (the cost of leaving where momentum carries each point, plus the fabric's stretch and bending) and moves each point to its own best spot in turn. On top, ever coarser copies of the cloth (each about half as fine) move whole regions at once, each with the region's real mass, by steps that can only lower that same energy (Galerkin multigrid, Lu et al. 2025). They carry a pin's pull across a fine sheet in a few steps, so a 10,000-point sheet stretches about 0.6% instead of 10%, and a sagging sheet settles as calmly as with XPBD. It costs 8-16 times XPBD on the CPU; on the GPU, where it also runs, a few milliseconds a tick.
- **Cloth on the GPU** (on by default, "Cloth on GPU" in F5): the same solvers ported to compute shaders (XPBD, and vertex block descent with its coarse levels), one thread per point or constraint, checked against the CPU's to within microns a tick; a 40,000-point sheet takes under a millisecond a tick, 17 times faster than on 16 CPU threads. The cloth is drawn straight from the GPU (its normals and tangents worked out there), and its points come back a couple of frames later for grabbing it with the mouse. Switch it off to compare with the CPU solver; headless runs and tests always use the CPU.
- **Deforming meshes** (for cloth): meshes whose vertices move every tick, drawn blended between ticks with their normals and tangents worked out again each frame. They cast shadows, show up in ray-traced light and reflections, and give temporal anti-aliasing their motion.

### The engine core

- **Entities and components:** generational entity handles and sparse-set component storage. Systems are plain functions that may destroy entities and remove components mid-walk (queued until the walk ends).
- **A fixed 120 Hz simulation** with interpolated rendering, so movement is smooth at any frame rate. Orientations are quaternions, blended along the shortest turn.
- **Deterministic:** the same commands give a bit-identical result, checked by a test that plays the demo twice. Ready for replays and networking.
- **A job system:** a team of worker threads, one per core, that split loops across every core by work stealing, with results that do not depend on which thread ran what. The first user, a lighting table built at start-up, went from 104 ms to 10.5 ms on 16 threads.
- **Event-based input** with engine-owned key codes, so no SDL types appear in game code.
- **Three camera modes,** shared by every game that wants them:
  - first person: a body that walks, jumps, crouches and sprints through the level with Overwatch-like movement
  - MOBA-style top-down with click-to-move
  - a free debug camera

### Physics and movement

- **Collision shapes:** spheres, capsules and axis-aligned boxes, with exact overlap tests between every pair, built on closest-point queries between points, segments and boxes.
- **Raycasts** against the shapes and against triangle meshes, with the hit distance, point and surface normal.
- **`Collider` components** (a shape plus optional exact triangles), and mouse picking that finds the nearest collider under the cursor in two stages: shapes first, then triangles to confirm.
- **A uniform grid broad phase,** so queries only test colliders near them.
- **Static level collision:** the level's triangles in their own grid, built once, with raycasts and capsule contacts (how deep, and which way out).
- **A kinematic character controller** tuned to feel like Overwatch. Walls slide, steps and ramps are climbed, the ground is followed down, gravity has air drag up to a terminal speed, and jumps and air steering use Overwatch's measured numbers.
- **Sphere sweeps:** the first touch of a moving ball, so nothing fast can pass through a wall.
- **Particles:** balls that fly on exact parabolas, bounce with restitution and Coulomb friction, slide to a stop and rest without jitter. The demo throws them as grenades.

### The games

Several games run on one engine, picked at start-up with `--game`.

**The demo:** a night scene with lamps, floodlights, a gallery of glTF test models (normal maps, a water bottle with a glowing label and baked occlusion) and an obstacle course.

**The light lab:** a sunlit test ground 800 m across.

- Towers at doubling distances, a thin chain-link fence running to the horizon, a forest, and a two-room building lit through one glazed window.
- A 300 m street of houses, whose 60 lamps and lit windows come on at dusk.
- A sun placed by real astronomy, from latitude, day of the year and time of day, that can run through a day. Its glowing disc in the sky is as bright as the light it gives, drawn five times its real size for looks (as is the moon's).
- At night, the moon in its real place and phase, lighting the sky and casting shadows, and the 9,096 brightest real stars, in their colours, turning with the sky.
- A thin ground mist, so sunbeams through the window, the flashlight's cone and the street lamps' halos show (Mist in F5).

**The cloth lab** (`--game clothlab`): a plain floor under an afternoon sun, with a test station for each of the cloth milestones: a ball on a pedestal under a bar, a mat to drop sheets onto, and a wide bar to hang two fabrics side by side. A cream cotton sheet pinned under the first bar falls and settles draped over the ball. A red cotton sheet hangs from two hooks on the wide bar; slide them together with Hook gap (F5, Hooks) to bunch it into folds. Hold the left mouse button on any sheet to drag it; it slides over the ball, posts and floor with cotton's friction (the fabric in F5, Cloth). Big sheet (F5, Stations, read at start-up) adds a 10,000-point sheet sagging like a hammock over the mat, the solver's heaviest work, on every core; it keeps up with real time with the default isometric bending, but not with the hinge's (Bending model, F5, Cloth). Left of the drape station a mannequin stands (a 1.75 m human figure; Mannequin in F5's Stations, read at start-up, on by default), and a dress is sewn onto it: a sleeveless A-line dress to the knee, its front and back cut from a pattern sized to the mannequin (12,000 points), laid round it and sewn at the sides and shoulders in its first second, weightless, then let fall. Its shoulder straps are pinned and carried onto the shoulders as it sews, as a dress is pinned on a dress form, and the rest hangs from them on tethers; with Dress pinned at the shoulders off (F5's Stations, read at start-up) it is held up by nothing but contact with the shoulders.

### Tools for building and testing

- **Settings in one place:** every tunable value (simulation rate, frame cap, light and shadow tuning, bloom, tone mapping, HDR output, camera speeds and mouse sensitivity, grenade physics) lives in a settings struct owned by its system. A `visit()` next to it describes each value once (label, limits, unit), and that drives everything else:
  - **a debug UI** (F5, Dear ImGui): a slider, checkbox or list for every setting, Save and Reset buttons, drawn over the image in its own layer so it looks right in HDR too
  - **quality presets** Low, Medium, High and Ultra (see the table below)
  - **a settings file** (`settings.cfg`): only the values changed from the defaults, loaded at start-up, forgiving of hand edits
  - **tests** that every default lies within its limits
- Numbers the shaders share with C++ are written once.
- **Profiling** in the F4 view:
  - frame, work, tick, update, scene-building and render times, and what each demo system costs per tick, as the average and worst call over each half second
  - the GPU's own time for each render pass, measured with GPU timestamps: the cloth, the ray tracing scene's build, the light probes, cluster light lists, shadows, the fog's grid, depth prepass, ambient occlusion, mirrored lights, main, the ray tracing debug view, the velocity pass, the depth measurement for the sun's cascades, the reflection probe's capture, the reflections, the fog over the picture, glass, the TAA resolve, eye adaptation, bloom, tone mapping, debug lines and the debug UI
  - the GPU cloth's time by kind of pass (prediction, the solvers' passes, the coarse levels' passes, the stitches, shaping its mesh), while cloth runs on the GPU
  - render passes and images carry names, so RenderDoc and Nsight show the frame as named passes
- **Debug drawing:** lines, boxes, spheres and capsules drawn over the scene, with an F4 debug view.
- **Unit tests** that run without a GPU or window.
- **Golden images:** fixed views of each game rendered once as references (the scene image in half floats and the final image, SDR and HDR). Later captures must match them pixel for pixel, with a difference heatmap when they do not.
- **A GPU test tier:** the shaders' maths lives in libraries of pure functions that the render passes and small compute shaders share. `make gputest` runs those functions on the GPU over thousands of fixed inputs and compares them with their tested C++ mirrors, then runs the golden images.
  - Tolerances are sized for how GPUs round (and, for sharp highlights, for how sensitive the maths itself is).
  - The report shows how much of each tolerance the worst value used.
- **A software rasterizer,** kept as a tested reference (lines, triangles, clipping, depth buffer).

**The quality presets**

| Preset | Sun cascades | Light probe rays | Sharing between probes |
|---|---|---|---|
| Low | 2048 | 32 | 0.5 |
| Medium | 2048 | 64 | 0.3 |
| High | 4096 | 256 | 0.1 |
| Ultra (the default) | 4096 | 1024 | none |

## Requirements

The project is developed on **Windows** with **MSYS2** (UCRT64 environment). You need:

| Software | Used for |
|---|---|
| [MSYS2](https://www.msys2.org/) | Shell, `make` and package manager |
| GCC with C++20 support | Compiler (`g++`) |
| SDL3 and SDL3_image | Window, input and image loading |
| shaderc (`glslc`) | Compiling GLSL shaders to SPIR-V |
| Vulkan headers and validation layers | Building the renderer, and checking it in debug builds |
| A GPU and driver with Vulkan support | Running the game (not needed for the tests) |

Everything else is included in `external/`, so it needs no install: cgltf, Dear ImGui, MikkTSpace, volk, vk-bootstrap, the Vulkan Memory Allocator, AMD's FSR 3.1 (headers and its signed DLL, which the build copies beside the game) and NVIDIA Streamline's headers. DLSS's DLLs are the one thing to fetch, and only for DLSS (step 4 below).

## Install

1. Install [MSYS2](https://www.msys2.org/) using its installer (the default location is `C:\msys64`).
2. Open the **MSYS2 UCRT64** shell from the Start menu, update MSYS2 and install the packages. Run the update twice: the first run updates the core and closes the shell, so reopen it.

   ```sh
   pacman -Syu --noconfirm
   pacman -S --needed make \
       mingw-w64-ucrt-x86_64-gcc \
       mingw-w64-ucrt-x86_64-sdl3 \
       mingw-w64-ucrt-x86_64-sdl3-image \
       mingw-w64-ucrt-x86_64-shaderc \
       mingw-w64-ucrt-x86_64-vulkan-headers \
       mingw-w64-ucrt-x86_64-vulkan-validation-layers
   ```

3. Clone the repository:

   ```sh
   git clone https://github.com/datslok/GameEngine.git
   cd GameEngine
   ```

4. Optional, for DLSS on NVIDIA RTX GPUs: fetch NVIDIA Streamline's DLLs (about 276 MB to download, needs a real Python). `make` then copies them beside the game.

   ```sh
   python tools/fetch_streamline.py
   ```

`glslc` from the Vulkan SDK also works if it is on your `PATH`.

## Build and run

Run these from the **repository root** in the UCRT64 shell. The game loads assets and shaders by relative paths (`assets/...`), so it must be started from there.

**Building and playing**

| Command | What it does |
|---|---|
| `make` | Builds `build/game.exe` and compiles the shaders to `.spv`. This is the unoptimised debug build, best for a debugger |
| `make run` | Builds, then starts the game (the light lab) |
| `make release` | Builds the optimised release (`-O2`) into `build/release/`. Physics runs about 7 times faster than in the debug build, so use it to play and to measure |
| `make run-release` | Builds the release, then starts it |
| `make clean` | Removes the compiled objects, executables and shaders |

**Testing**

| Command | What it does |
|---|---|
| `make test` | Builds and runs the tests (`build/tests.exe`) |
| `make test-release` | Builds and runs the tests optimised, which can catch bugs the debug build hides; the quicker of the two, the one to run routinely |
| `make test-slow`, `make test-slow-release` | Runs the slow tests left out of the others: vertex block descent and its coarse levels (about half a minute in release) |
| `make golden` | Renders every game's golden views and compares them with the references in `tests/golden/<GPU>/<game>/`. Needs a GPU; captures and difference heatmaps go to `build/golden/<GPU>/<game>/` |
| `make golden-save` | Renders the golden views and saves them as the new references. Only on purpose, after a change that is meant to alter the picture |
| `make yardstick` | Measures the cloth solver: a 1 m sheet at four resolutions, its stretch, sag and cost a tick (release build; `docs/CLOTHGPU.md` holds the numbers) |
| `make yardstick-quick` | The same in about a minute, for development: the CPU rows only, three resolutions, settled less |
| `make gputest` | Runs every shader library's functions in compute shaders on the GPU and compares them with their C++ mirrors (`build/gputests.exe`), then the golden images. Needs a GPU; `make gputest-release` for the optimised build |

**Good to know**

- `GAME=` picks the game: `make run GAME=demo` starts the demo. It works for `make run-release`, `make golden` and `make golden-save` too.
- Only changed files are recompiled, in parallel on every core.
- New `.cpp` files in `src/` and `tests/` are picked up automatically.

### Building from PowerShell or another terminal

Outside the UCRT64 shell, put the MSYS2 tools first on your `PATH` for that session:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;" + $env:PATH
make run
```

The game also needs `C:\msys64\ucrt64\bin` on the `PATH` when you start it directly, because that is where `SDL3.dll` and the other runtime libraries live.

## Sharing the demo

To send the demo to someone without MSYS2, run:

```sh
python tools/package_demo.py
```

It builds the game, then creates `build/GameEngineDemo.zip` with:

- the exe, and a launcher per game, `LightLab.bat`, `Demo.bat` and `ClothLab.bat`, each starting the exe with its `--game` (the exe alone starts the light lab)
- every DLL it needs, found automatically, including the image decoders SDL3_image loads at runtime
- the assets
- a short README with the controls, copied from the Controls section below, so it never falls behind

Before zipping, it checks that every DLL resolves inside the package or Windows itself.

**Options**

- `--no-build` packages the existing build.
- `--anonymous` makes `build/Demo.zip`, for sharing anonymously: a neutral name, a README with only short control names (nothing that matches this repo's text), every zip entry dated 1 January 1980 and the exe's build time zeroed.
- `--msys-root` points to MSYS2 if it is not in `C:\msys64`.

It needs a real Python 3 (standard library only; `winget install Python.Python.3.13`). The `python` in `WindowsApps` is only the Microsoft Store placeholder.

## Controls

The game starts in first-person mode. F1-F3 switch modes at any time.

The light lab is the default game (`--game lightlab`; the exe alone in the zip). It has the same controls for moving and looking (F1-F5). The cloth lab (`--game clothlab`) has them too. The field of lamps and the grenades (G) are the demo's only.

| Input | Action |
|---|---|
| **F1** | First-person mode |
| **F2** | MOBA mode |
| **F3** | Free camera mode |
| **Alt+Enter** | Toggle fullscreen |
| **Escape** | Release the mouse |
| **F** | Toggle the flashlight (off at the start, held in your right hand in every mode; in MOBA mode the duck carries it) |
| **L** | Toggle the light lab's back-room ceiling lamp (off at the start), or the demo's field of 100 coloured lamps |
| **K** | Toggle the light lab's street lights, or the demo's four floodlights over the obstacle course (on at the start) |
| **Left mouse (held)** | Drag the cloth lab's sheet: grab it where the crosshair points (the cursor in MOBA mode), within 10 m, and pull; let go to drop or fling it |
| **F4** | Debug view: timings, bounding boxes, colliders, culling and light counts, and video memory (listed below) |
| **F5** | Settings window (debug UI): a slider, checkbox or list for every engine and game setting, with quality presets, Save and Reset |

**The lights**

The flashlight (F) is a fast light: its bounce is kept apart with a short memory, and switching it tells the light probes, so its bounce comes and goes with it. Shine it into the light lab's dark back room to watch its bounce light follow it.

The back room's ceiling lamp (L) is a still light. Its bounce comes in at once when switched, since the switch tells the light probes round it to start afresh. Moving the clock by hand does the same for every probe.

The light lab's 60 street lamps and lit windows come on by themselves at dusk and go off at dawn. K flips them until the sun next does.

The demo's 100 lamps (L) fly round their pillars like fireflies, so the pillars' shadows sweep round. They are more than the 64 point light shadows, so light priority has to choose which lamps cast them: a stress test for light priority and shadows, since every shadow tile is redrawn each frame while its light moves. They are fast lights, so their bounce light on the ground and pillars flies with them.

The demo's floodlights (K) are shadow-casting spotlights on poles, whose crossing beams fill in each other's shadows, like stadium lights.

**The light lab's settings**

Its sun is under Light lab, Sun in the settings window (F5): latitude, day of the year, solar time, hours per second to run the clock, and the sun disc's size (1 is the real half degree).

Its light probes follow the camera everywhere. How many and how dense is under Global illumination in the settings: Cascades, Probe spacing, Probes across and high, and Seed starting probes.

**The F4 debug view**

- Timings in pale green: frame, work without the frame cap's sleep, one tick, the camera update, building the scene, and rendering (which includes waiting for the GPU), as average/worst milliseconds.
- Then the GPU's time for the whole frame and for each pass, averaged, the GPU cloth's by kind of pass (only the kinds that ran), and each simulation system's average cost per tick.
- Bounding boxes around every mesh, collider wireframes in orange, and a marker at each light.
- How many colliders the hover ray tested out of all of them (the broad phase at work), and the level triangle under the cursor with its normal and how many level triangles that ray tested.
- The duck's ground normal, slope and movement substeps, and the walk target in MOBA mode.
- How many meshes culling skipped, and how many draw calls each view took (CALLS).
- How many point lights are in view, lit and shadowed, and how many shadow tiles were drawn, kept from earlier frames or skipped, with how far the sun's cascades reach (SUN TO).
- The video memory in use against the driver's budget, with the engine's own share (VRAM, top-left).

**The F5 settings window**

The settings are grouped by system (light priority, shadows, bloom, tone mapping, HDR output, frame cap, cameras, grenades). Save writes the settings you changed to `settings.cfg`, loaded at every start. Greyed-out settings are read only at start-up. The mouse is freed while the window is open, and clicks on it do not reach the game.

**First person and free camera**

| Input | Action |
|---|---|
| Left-click | Capture the mouse for mouse look |
| Mouse | Look around |
| W A S D | Move: in first person you walk through the level at Overwatch's speeds (5.5 m/s, 90% backwards), climbing steps and ramps and stopped by walls |
| Space | Jump (first person; a fresh press each time) |
| G | Throw a grenade (first person): it flies along your view, bounces off floors, walls and steps, slides to a stop and is gone after 5 seconds |
| Ctrl (hold) | Crouch in first person (3 m/s, stays crouched under low ceilings); move down in free camera |
| Space / Ctrl | Move up / down (free camera only) |
| Shift (hold) | Sprint in first person (1.5 times), fly twice as fast in free camera |
| F1 from free camera | Drop into first person where the camera is, and fall to whatever is below |

**MOBA**

| Input | Action |
|---|---|
| Mouse over an object | Outline it faintly (hover) |
| Left-click | Confine the cursor to the window; once confined, select the object under the cursor (outlined in cyan; clicking bare ground deselects). Objects are picked by their exact triangles, so a click through the gap in the teapot's handle misses it |
| Right-click (or hold) | Move the character to the cursor: it walks over the level, up steps and ramps (no path finding yet, so walls stop it) |
| Cursor at the screen edge | Pan the camera |
| Space | Lock or unlock the camera on the character |

## Project layout

```
src/              Engine and game source, in layers (lower ones never include higher ones):
  app/            main and the game registry: picks a game by name and starts the engine
  games/          Games built on the engine: games/demo (the demo), games/lightlab (the sunlit test ground)
                  and games/clothlab (the cloth test ground)
  engine/         The main loop, the Game interface, fixed-tick updates, the debug UI and the settings file
  cloth/          Cloth: its points (positions, velocities, inverse masses from the fabric's mass per area), their step,
                  the tethers and coarse levels that keep fine sheets from stretching, and sewing patterns:
                  flat pieces cut into cloth and sewn together
  gameplay/       Character movement, spinners, fireflies, grenades, camera controllers
                  and the player controls games share (F1-F3)
  platform/       The window and SDL input translation
  render/gpu/     The renderer's parts that do not call Vulkan:
                  - physically based shading and normal mapping, and their tested CPU mirrors
                  - shadows: the atlas and its caching, the sun's cascades and their fit to the screen's depths,
                    soft shadows
                  - light priority, debug lines, the overlay the debug UI is drawn into, quality presets
                  - the GPU scene: the instance, material and light records the shaders read,
                    and draws batched by mesh
                  - the depth tests of the depth prepass and main pass
                  - clustered shading's grid, light tests and heatmap colours
                  - ray tracing's instances, masks and debug view camera
                  - the light probes' maths: octahedral images, ray directions, blending, the atlas layout,
                    the cascades that follow the camera and their scrolling, and sampling them at surfaces
                    (the cell, the visibility and back-face weights, the cascades' blend, the lights kept
                    at rays' hits)
                  - the sky and atmosphere, the stars, tone mapping, eye adaptation and night vision, bloom
                  - volumetric fog: the height fog, the froxel grid, the light scattered in the air, the
                    fog along a ray slab by slab
  render/vulkan/  The engine's own Vulkan 1.3 renderer:
                  - the device (with or without a window), swapchain, barriers, graphics and compute pipelines
                  - per-frame uniforms and descriptors, the bindless texture array, the scene's buffers and layout
                  - ray tracing's acceleration structures
                  - every pass: the light probes' rays (with their bounce and lamps) and update, the clusters'
                    light lists and the fog's grid (compute passes); shadows, depth prepass, main, the probes' debug balls,
                    the ray tracing debug view, the depth measurement, bloom, tone mapping, debug lines,
                    the debug UI
  render/software/  Software rasterizer, kept as a tested reference
  scene/          Meshes (with tangents for normal maps), models, materials, lights, fog, light probe placement
                  (cascades that follow the camera) and events, the sun's, moon's and stars' places in the sky,
                  the camera, render components, interpolation and the frame description
  assets/         The asset manager, and OBJ, glTF, image and star catalogue loaders
  input/          Engine keys and the input snapshot (no SDL)
  ecs/            Entities, component storage and the World
  core/           Fixed timestep, job system, profiler, sRGB conversions, the settings visitor, float tolerances
                  and small shared types
  physics/        Collision shapes, overlap tests, raycasts, colliders, picking, the broad phase grid,
                  static level collision, sphere sweeps, particles, the character controller and
                  bodies' signed distance fields
  math/           Vectors, matrices, quaternions, transforms, rays, boxes, closest points, smoothstep,
                  Delaunay triangulation
tests/            Unit tests (one test function per file, registered in tests/test_main.cpp)
  golden/         Golden image references, one folder per GPU and in it one per game
                  (see tests/golden/README.md)
  gpu/            The GPU test tier (make gputest): the runner, one test per shader library,
                  and their compute shaders in gpu/shaders
docs/             ENGINE_GUIDE.md: how the engine works and why, for people learning it
                  ARCHITECTURE.md: the detailed reference of every system (names, files, numbers)
                  BUILDLOG.md: the history of finished roadmap items (when, why, measurements, dead ends)
                  ILLUMINATION.md: research notes for global illumination (DDGI papers, formulas, numbers)
                  LUMEN.md: research notes on Lumen and its alternatives, and a plan for this engine
                  RAGDOLLPLAN.md: plan and research notes for active ragdolls (roadmap phase 8)
tools/            package_demo.py (the shareable zip), fetch_streamline.py (DLSS's DLLs), make_glb_demo.py
assets/
  models/         OBJ and glTF test models (the gallery: Khronos's CompareNormal,
                  NormalTangentTest, NormalTangentMirrorTest, WaterBottle and AlphaBlendModeTest)
  shaders/        GLSL shaders (compiled to .spv by make) and the shader libraries they include
                  (.glsl: BRDF, normal mapping, lighting, soft shadows, cascades, depth range, ray tracing,
                  light probes, atmosphere, stars, eye adaptation, tone mapping, bloom, dither, overlay, clusters)
  stars/          The star catalogue
  textures/       Images
external/
  cgltf           Vendored glTF loader
  fidelityfx      Vendored AMD FidelityFX API headers and signed FSR 3.1 DLL (v1.1.4)
  streamline      NVIDIA Streamline's headers (v2.14.16); its DLLs fetched into bin/ (git-ignored)
  imgui           Vendored Dear ImGui (the debug UI), with its SDL3 and Vulkan backends
  mikktspace      Vendored MikkTSpace (tangents for normal maps, C)
  volk            Vendored volk (loads Vulkan's functions from the driver, C)
  vk-bootstrap    Vendored vk-bootstrap (Vulkan instance, GPU and device set-up)
  vma             Vendored Vulkan Memory Allocator
build/            Build output (git-ignored)
```

## Troubleshooting

- **The game cannot find a model, texture or shader:** start it from the repository root, or use `make run`.
- **`SDL3.dll` was not found:** add `C:\msys64\ucrt64\bin` to your `PATH` (see above).
- **The glTF tests fail with a "directory exists" error:** an earlier failed run left its temp folder behind. Delete the `game_engine_gltf_*` folders in your temp directory (`%TEMP%`) and run again.
- **`make: command not found` in PowerShell:** `make` is part of MSYS2, not Windows. Use the UCRT64 shell, or set the `PATH` as shown above.

## Credits

- [cgltf](https://github.com/jkuhlmann/cgltf) by Johannes Kuhlmann, for glTF parsing
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut, for the debug UI
- [MikkTSpace](https://github.com/mmikk/MikkTSpace) by Morten S. Mikkelsen, for tangents (zlib licence)
- [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) v1.1.4, for FSR 3.1 upscaling (MIT licence)
- **NVIDIA DLSS**, through [NVIDIA Streamline](https://github.com/NVIDIA-RTX/Streamline) v2.14.16 (Streamline MIT; DLSS under the NVIDIA RTX SDK licence). NVIDIA, the NVIDIA logo and DLSS are trademarks of NVIDIA Corporation.
- The Yale Bright Star Catalogue, 5th revised edition (Hoffleit and Warren 1991, from the [CDS](https://cdsarc.cds.unistra.fr/viz-bin/cat/V/50)), for the stars (`assets/stars/`)
- [SDL3](https://www.libsdl.org/) and SDL3_image
- The duck model and the gallery are from the [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets): WaterBottle by Microsoft (CC0), CompareNormal by Eric Chadwick and DGG (CC0), NormalTangentTest, NormalTangentMirrorTest and AlphaBlendModeTest by Ed Mackey, © 2018 Analytical Graphics, Inc. ([CC BY 4.0](https://creativecommons.org/licenses/by/4.0/))
