# Filling the valley

Research for phase 18's "the rest of the valley, keeping the sight lines to the Monolith and the Ring" (Nov 19 2026). Tags: [V] read in a primary source (talk, paper, official docs), [S] secondary, [K] background, not checked.

## Where we are

- Planted today: about 4.4 ha between the Ring and the clearing (z -210 to -40, x ±130 m): about 1,290 trees (10 emergent, 80 canopy, 1,200 mid asked for, darts on the CPU at start-up), about 10,000 understorey scans on a 1.5 m grid, palms on 5 m, lianas from 80 % of crowns; grass and floor cover in 8 m cells within 200 m of the path. Pillar and island tops: nothing planted.
- The valley: a floor 400 m wide along the axis from the rim (z 790) to past z -5000, walls to a 150 m rim, mountains outside; 220 pillars, 30 islands, the Bastion and ten far towers. Far plane 25 km.
- Sight lines today: viewpoints every 10 m along the path, its end, the clearing and rings round meadow and clearing, each casting a segment to 250 m up the Bastion and to the Ring's top; a crown meeting one is not planted.
- Scale machinery: still draws in 128 m cells with lasting GPU buffers, 64-slot chunks, GPU culling of every view (CULL 0.19-0.25 ms), mesh megabuffer (about 12 draw calls), levels by pixel error, hemi-octahedral impostor cards (trees switch about 1 km out), shadow stand-ins, card thinning, plant fades, a 5 km far shadow map, distant occluders.
- Numbers (laptop RTX 4070): 76,654 slots for about 49,000 draws; forest meshes 502 MB; start-up 25 s (10 s of it the first frame's CPU); frames GPU-bound (main about 9 ms, AA 3.7, cube capture 3.1 before GPU culling; from the rim 18.5 ms average). Sources: `docs/buildlog/phase-18-2.md`, `tools-1.md`, `WORLDSCALE.md`.
- Scaling the patch as it is to the floor alone (about 230 ha) gives about 67,000 trees and 500,000 understorey plants, about 800,000 draws: ten times today's records, and the start-up darts and entities with them. Not the way.

## How shipped games place vegetation over square kilometres

| Title | What is stored | What is made at runtime | Numbers |
|---|---|---|---|
| Horizon Zero Dawn (GDC 2017, van Muijden) [V] | only 2D "world data" maps, painted or derived (height, water, ecotopes, placement maps at 0.5-2 m), about 4 MB a km² | every instance, on the GPU, round the player: a graph per layer turns the maps into density, dithered against an ordered pattern scaled to the layer's footprint (trees 6 m, undergrowth 1 m); layers sharing a footprint take density bands so they never collide | trees in 128 m blocks, grass 32 m; a fixed 64 x 64 density map a block; about 250 µs of GPU on average, 100,000+ objects round the player, 500+ asset types; 3 people made the nature, 1 the ecotopes |
| Far Cry 5 (GDC 2018) [S] | Houdini biome recipes baked in the editor | — | species compete for ground; denser on north slopes; grass by humidity and wind maps |
| Ghost Recon Wildlands [S] | trees and bushes placed offline in Houdini | grass from the terrain's materials | cascading seeding: big trees seed medium, medium small; slope, spacing, sun, masks for rivers and cliffs; roads and paths cleared |
| Unreal 5 PCG [V] | graphs | cells generated round sources, a radius a grid size, larger grids for trees, smaller for grass; small layers read the trees' points | defaults 16 cells in flight, 16.7 ms frame budget; forum: 1 ms spikes |
| Ghost of Tsushima grass [S] | per-tile type and height textures | blades laid out per tile in compute, culled, drawn instanced | 15 and 7 vertices a blade |
| Flight Simulator [S] | — | about 2 trillion trees from aerial imagery; authors paint vegetation and exclusion polygons | about 14 species |

- **Ecology** [V]: EcoBrush (Gain et al. 2017) synthesises 44,000 plants a km² in under a second from canopy-spacing statistics; Synthetic Silviculture (Makowski et al. 2019) simulates tropical gap dynamics (palms fill a fallen tree's gap first). Practical part for games: spacing by crown radius, each lower layer reading the one above as shade. Real rainforest: about 400-700 stems over 10 cm a hectare [K]; our patch about 290 a hectare.
- **Landmarks** [S]: Ghost of Tsushima removed trees that hid its landmarks and raised flags over the canopy; Horizon's artists paint tree lines and the placement follows; Unreal and Wildlands cut paths, roads and clearings by masks.

## How shipped games draw far forests

| Title | Near | Middle | Far |
|---|---|---|---|
| Serious Sam 4 (Croteam, GDC 2019) [V] | trees on a jittered 8 m grid, 1 km blocks of about 16,000 | a 2-triangle impostor a tree, prebuilt blocks of 128 x 128 quads; 8 directions, perspective corrected per pixel, the nearest two blended; albedo, subsurface, normal, AO and depth (for its shadows) | a "far forest" material splatted into the terrain; vistas 50-100 km |
| Horizon Zero Dawn [S] | SpeedTree models, own LOD chains | cross-plane billboards at LOD 4-5 | — |
| Ghost of Yōtei [V] | — | GPU culling: over a million far trees, rocks and bushes culled to about 60,000 a frame | far terrain textures baked from the models |
| Ghost Recon Wildlands [S] | | impostors baked in engine, switching sooner where the forest is denser | |
| X-Plane 12 [V] | a few hundred real trees | camera-facing billboards, cross-faded, LOD and wind on the GPU | millions of trees |
| Unreal 5.7 Nanite foliage [V] | trees as instanced parts (assemblies), skinned wind (100,000 bones 0.1 ms) | clusters turn to voxels where that is the smaller error | Witcher 4 demo: 500,000 tree instances |

- **Shadows** [V/S]: Horizon draws vegetation into only its two nearest cascades, each with one fixed shadow level for everything in it (stopped popping), from separate cheaper casters; shadows about 9 % of its frame [S]. Unreal: anything moving its vertices invalidates cached shadow pages, so wind is turned off past a distance (one forum: shadow depths 20 to 2 ms); past the cascades, distance-field shadows or none plus contact shadows. Serious Sam 4's impostors carry depth so they cast. Ubisoft's virtual shadow maps cut shadow triangles 30-80 %.
- **Stability** [V]: alpha mips keeping coverage (Castaño), hashed alpha (Wyman 2017), mip bias log2(render/display) under upscaling, dithered level cross-fades under TAA.
- **Not published:** impostor switch distances or vegetation milliseconds for any of RDR2, Witcher 3, Far Cry, Flight Simulator.

## What this means for this engine

- **Store maps, not plants.** A density map a layer over the valley (Horizon): derived from the terrain (floor, slope, rim, height above the floor, distance to the path), painted by masks for clearings and sight corridors, a few MB. The plants come from the maps by an ordered dither at each layer's footprint, seeded by place, so any cell is the same every time it is made (deterministic, testable on the CPU).
- **Make plants near the camera, by cells.** Trees in 128 m cells (our still-draw cells already are), plants in 32 m; a radius a layer; cells made and dropped as the camera moves, on the jobs, a few a frame. Cost grows with the cells round the camera, not the valley. The patch's darts and sight lines become the same maps, so the patch looks the same.
- **Three tiers, as Serious Sam 4:** real trees and plants near (today's meshes, levels and fades); past about 1 km, where our trees are already impostors, an impostor field per cell with no entities (position, variant, turn, scale per tree, one indirect draw, culled by cell); past a few km, the canopy in the terrain's colour and normal. The middle tier is the one new drawing path; it lets tens of thousands of trees stand where 1,290 do now.
- **Shadows:** trees and plants only in the nearest cascades with a fixed level each (Horizon); impostors cast into the far shadow map from their depth (Serious Sam 4); sway already fades by 400 m, so far cascades stay cached.
- **Sight lines as corridors:** the segments from designed viewpoints (path, clearing, rim, pillar-top lookouts later) stamped into a mask that caps the canopy's height under each, as Ghost of Tsushima cleared its landmarks' views; not every viewpoint in the valley.
- **The frame is already GPU-bound:** each stage measured (F4) before the next; the levers in `WORLDSCALE.md` (per-view budgets, opaque far leaves, density by distance) join where a stage needs them.
- **GPU placement** (Horizon's, generating instances in compute straight into the draw lists) is the end state for scale. CPU cells first would reuse every system the trees already have and test without a GPU; the user chose the GPU straight away (below).

## The user's choices (Nov 19 2026)

- **Extent:** the floor, walls and rim (not the mountains outside).
- **Look:** ecotopes, each its own density: dense floor jungle, sparser on the walls' slopes, rim forest, gaps by noise.
- **Sight lines:** corridors from designed viewpoints (path, clearing, rim, later pillar-top lookouts) capping the canopy's height under each line.
- **Placement straight on the GPU** (Horizon's way), not CPU cells first: the stage's design (how compute-made instances join the GPU scene's records, culling, levels, shadows and sway) to be explained before building.
- Stages on `ROADMAP.md` under the forest: `valley-maps`, `gpu-placement`, `impostor-fields`, `far-canopy`, `valley-tops`.

## Stage 2's design (`gpu-placement`)

- **Who decides where (the user's choice, Nov 19 2026): the GPU, every layer.** Weighed against the CPU placing per cell and the GPU only building: the CPU's cost grows with ground entering range a second times density (estimates, a few µs a plant: ~10k plants/s at 100 m/s, ~100k/s at 300 m/s with impostors to ~5 km, a teleport's whole radius at once), the GPU's stays well under a ms; the CPU form is exact and smaller to build, the GPU form keeps scaling for phase 15's flight and the far tiers.
- **A CPU mirror only where the CPU uses positions:** trees within a few hundred metres of the player (lianas, colliders later, `forest-climber`, `forest-epiphyte`, the start). Ferns, palms, floor cover and far trees have no CPU copy.
- **Agreement by construction:** the decision maths (shares, thresholds, fits, spacing) in adds and multiplies marked `precise` (no fused multiply-adds), trig (the valley's axis) and other approximated functions read from tables both sides share, the 64-bit hash kept (`shaderInt64`), so the patch stays as it is; a GPU test counts disagreements. Where one slips through near the player, the CPU's tree carries the lianas and collider.
- **Layers in order per cell:** a pass a layer over its cell plus a border of the next layer's reach (the canopy reads the emergents standing, the mid layer their trunks, the understorey every trunk).
- **One record a plant (the user's choice):** one 32 B record a plant (position, turn, scale, variant) and a part table a variant (mesh, material, sway, shadow cascades), culled once a plant a view (all parts already choose by the tree's box), the vertex shaders building the matrix: ~3 MB for ~100k plants round the camera against ~140 MB as full records a part (~7 draws a tree at ~300 B).
- **Lianas (the user's choice):** lianas made on the CPU per near cell from the mirror's trees, as today.
- **Cost grows with:** GPU memory with the plants within the radii; GPU time with plants in cells passing each view; placement with the cells made a frame (a cap).
- **2b's steps (the user's choices, Nov 19 2026):** b1 places cells on the GPU and compares them with the CPU before anything is drawn; b2 draws the placed trees over the patch only, behind a switch beside the entity trees, so the two compare like with like (the whole valley waits for 2e's measurements); the entities retire only after 2c gives the placed trees shadows, sway and motion.

## Drawing the whole valley: distances, far shadows, probes (e1, Nov 20 2026)

Trees to 1 km took the GPU frame from about 17 to 54 ms (laptop RTX 4070, DLSS at 0.67): MAIN 21.6, SHADOW 13.3, PREPASS 8.3, CUBE 4.0, PLACED culling 0.74. The mid layer (480 a hectare, 15 m tall) far outnumbers the canopy (30) and emergents (2.8); every tree's shadow stand-ins cast into every cascade. Few studios publish vegetation numbers; [V] read, [S] secondhand.

| Question | What shipped games and engines do |
|---|---|
| How far each kind is drawn | Battlefield 3 [V]: vegetation in overlapping quadtree payloads, far "high distance, low density" trees, nearer denser trees, then bushes, then detail ([Frostbite GDC 2012](https://media.contentapi.ea.com/content/dam/eacom/frostbite/files/gdc12-terrain-in-battlefield3.pdf)); Frostbite undergrowth only in 16 m cells near the viewer [V]; Serious Sam 4 bushes a nearer layer than its tree impostors, far forest in the terrain's colour [V]; Unity: tree distance, billboard start and a cap on mesh trees [V]; Unreal foliage start/end cull distance a type (default never) [S] |
| Culling by size | Unreal cull distance volumes: (size, distance) pairs, each object the pair nearest its diameter [S]; Unity 6 small-mesh screen percentage 0-20 % [S]; `r.Shadow.RadiusThreshold` culls cascade casters below 0.03 of the screen [S]; no shipped pixel thresholds published |
| Density by distance | BF3's payloads thin trees too [V]; Wildlands switches levels sooner where denser [V]; Unreal's density scale is global, not by distance [V]; survivors grown to keep cover: no published case for trees |
| Far shadows | Horizon: vegetation only in the 2 nearest cascades; Unreal: the far cascade draws only flagged objects (the landscape), foliage left out [S]; distance-field shadows past the cascades to 1.2 km, half resolution [V]; Witcher 3: a terrain shadow clipmap of heights, re-rendered on streaming or time of day, a few large meshes cast into it [S]; CryEngine: far cascades drawn once and reused, 16 km for about 130 MB [V]; RDR2 and Flight Simulator thin or drop far tree shadows [S] |
| Shadow stand-ins | CryEngine: 3,969-triangle roots cast through a 640-triangle stand-in, 5.4x fewer shadow triangles [V]; merged stand-ins a cell cast double shadows in Unreal's HLOD [S]; crown volumes as casters: none found |
| Probes | RDR2: one cube a frame at the camera, static opaque objects at low levels only [V]; Unity's culling mask for plants and GTA V's terrain-only faces (`WORLDSCALE.md`); none puts far trees in a dynamic probe |
| Cut-out cost | Unreal 4.17, 47,124 instances: masked 1.6x slower than opaque, opaque grass with 9x the triangles slightly faster than cut-out cards [V forum]; Unigine: dropping the prepass for dense near cut-outs is much slower [S]; impostors writing depth from a texture cast and receive shadows [V] |
| Budgets | BF3 terrain decoration 1-4 ms on PS3 [V]; Ghost of Yōtei over a million objects culled to about 60,000 [S]; no long-range tree milliseconds published |

**What it means here [inference]:**

- **A distance a layer:** the mid trees in the middle cells (300 m) with the understorey and palms, so past 300 m only the sparse tall trees stand (BF3's payloads, Serious Sam's split); cost grows with the plants within each layer's radius.
- **Trees cast only into the near cascades** (Horizon), their stand-ins' `shadowCascades` limited; the far forest's shadow later from one caster a cell or a canopy height field in the far shadow map (Witcher 3), drawn only when cells change or the sun moves (CryEngine's cached far cascades).
- **Probe faces without placed plants past a short range** (RDR2, Unity, GTA V): cost with the plants near the probe.
- **The prepass's far cards** (8.3 ms for depth alone): opaque far cards on hull meshes, or no prepass for single far cards; measured before building, since dense near cut-outs need the prepass.
- **Size culling** of casters and seen plants (Unreal's pairs): a few pixels on each plant's sphere in the cull pass; gain unmeasured anywhere.
