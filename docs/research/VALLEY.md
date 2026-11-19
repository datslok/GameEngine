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
