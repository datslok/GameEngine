# Level of detail for vegetation: research

Notes on how shipped games and engines draw dense foliage cheaply at a distance, gathered on Nov 15, 2026, before building phase 18's level of detail and impostor items. The forest then ran 23.5 ms of GPU among the trees on the RTX 4070 Laptop (DLAA).

Each part records what the sources say and then **what it means here**. Facts marked **(unverified)** are from memory or informal sources; check them before relying on them.

## What the forest pays for

Measured in a hidden copy at the trees' start (the RTX 4070 Laptop, TAA, render scale 1 and then 0.5, three readings each, twice):

| Pass | Scale 1 | Scale 0.5 (a quarter of the pixels) | Fixed part (geometry) | Per-pixel part |
|---|---|---|---|---|
| Main | 8.8 ms | 4.5 ms | about 3.1 ms | about 5.7 ms |
| Depth prepass | 1.7 ms | 1.55 ms | about 1.5 ms | about 0.2 ms |
| Reflection probe capture | 3.2 ms | 3.6 ms | about 3.3 ms | none |

- About 8 ms of the frame is geometry, which level of detail cuts. The probe's capture is all geometry: one small cube face a frame, every tree in reach at full detail.
- About 6 ms is shading, which level of detail barely touches; a smaller render size (DLSS Quality, the typical GPU setup) does.
- 1,290 trees come from about 11 meshes (3 kinds, 3-4 variants each) and about 10,000 plants from a few scans: few shapes, many copies, the easy case.

## Discrete levels in shipped games

**Horizon Zero Dawn** (Gilbert Sanders, "Between Tech and Art: The Vegetation of Horizon Zero Dawn", GDC 2018, [slides](https://media.gdcvault.com/gdc2018/presentations/gilbert_sanders_between_tech_and.pdf)), the most concrete numbers found:

| Asset | Level 1 | Level 2 | Level 3 | Level 4 | Level 5 |
|---|---|---|---|---|---|
| Trees | ~10,000 triangles | ~2,600 | ~1,200, cheap shader | ~200 + a 12-triangle billboard fading in | billboard |
| Plants | ~2,000 | ~800 | ~140, cheap shader | 8-triangle billboard | |
| Grass | 20-36 | 10-18 | 10-18, cheap shader | | |

- Each step keeps 25-45% of the triangles; a cheaper shader comes in at the middle level.
- The billboard dissolves in while the last mesh still draws.
- A depth-only pass first, then the shading pass with an EQUAL test ("zero percent overdraw"): this engine's prepass.
- Cut-out mips keep their coverage (this engine's `coveragePreservingMips`).
- Shadow casters of their own: cascade 0 the third level, cut-out and animated; cascade 1 fewer triangles still, neither cut-out nor animated.

**A production rule of thumb** (SpeedTree interview with Adrien Paguet-Brunella, store.speedtree.com): level 1 two thirds of level 0, then half each step, the last a flat fake.

**SpeedTree** (its runtime SDK's and modeler's level of detail pages; Unity's importer manual):

- One value per instance, 1 full detail down to 0 the lowest mesh; 0 to -1 fades the mesh out and the billboard in.
- Small outlying branches shrink onto their centre line, then go; inside parts go first.
- Leaves: **fewer, bigger cards.** Removed at set levels while the survivors grow to keep the leaf cloud's volume; "Jumble" picks the removed ones at random so the crown does not turn hollow.
- Tubes lose length and round segments ("too much length reduction can cause visible pops").
- Typical: 3-4 meshes and an optional billboard. No distance thresholds published.

**Unity's LODGroup:** thresholds as the object's height in screen heights, a global bias; fades by dither or SpeedTree's vertex morph, timed or over a share of the range.

**Unreal:** a screen size per level (the bounding sphere's projected diameter over the screen); foliage got dithered transitions in 4.7-4.8. Fortnite's Nanite advice (Epic's Chapter 4 blog, quoted in search results, **unverified** directly): opaque triangles beat cut-out cards for trees and grass once triangles are cheap; wind off past a distance.

**CryEngine:** small plants a single mesh and a sprite at a tuned distance; a dithered dissolve hides the switches (Mittring, SIGGRAPH 2007). The original Far Cry (2004) popped.

**Not found:** Far Cry 5, The Witcher 3 and Ghost of Tsushima's numbers (talks behind GDC's paywall). Ghost of Tsushima's grass blades about 15 vertices near and 7 far **(unverified)**.

**What it means here:**

- 3-4 meshes a shape, each 25-50% of the last, then an impostor.
- The bark is generated, so its levels come from the generator itself: fewer round and length segments, twigs below a radius dropped. Better than simplifying afterwards.
- The leaf cards' levels are SpeedTree's: drop a seeded random share, inside clumps first, and grow the rest to keep their area (sides times 1 / sqrt(kept share)). This also cuts the cut-out overdraw in the prepass.
- The scanned plants through meshoptimizer (below).

## Selection

- **Screen size** (Unreal, `ComputeBoundsScreenSize`): 2 R / (d · 2 tan(fovY / 2)), the bounding sphere's diameter over the view's height; a narrower field of view raises it.
- **Error in pixels** (Nanite, 3D tiles, meshoptimizer): e · H / (2 d tan(fovY / 2)), with e a level's error in metres; pick the coarsest level under a pixel budget (about 1 pixel for Nanite) **(the formula from general knowledge)**.
- H is the **render** height (under DLSS or TAAU, not the output's), times a global bias.
- Each view picks for itself: the reflection probe's small faces pick coarser levels on their own. Shadow cascades choose by the camera's distance, not the light's, so a tree's shadow matches the tree.
- **Hysteresis:** no source gives a number; engines hide flicker with timed or ranged fades. A 5-10% dead band is common practice **(unverified)**.

## Transitions

- **Popping:** free, fine when steps are small and TAA softens them.
- **Dithered cross-fade** (Unity, Unreal, CryEngine): both levels drawn during the fade, each discarding the other's pixels by a screen pattern turned each frame, which TAA or DLSS blends. A discard turns off early depth for that draw (Unreal's UE-25920). Here the main pass shades each pixel once anyway (EQUAL), so the cost is drawing the fading instances' geometry twice.
- **Vertex morph** (SpeedTree): each vertex knows its place in the next level; no double draw, but needs the generator to record it.
- **Cheapest with TAA:** pop or shrink parts between meshes; dither only the big step from the last mesh to the impostor (Horizon, SpeedTree).

## Impostors

**Epic's Impostor Baker** ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/impostor-baker-plugin-in-unreal-engine); Ryan Brucks' "Octahedral Impostors", 2018, which would not load):

| Topic | Epic's numbers |
|---|---|
| Fortnite's far foliage | 12 x 12 frames in a 2048 atlas, upper hemisphere for trees |
| Example | 16 x 16 frames in 4096 (256 pixels a frame), captured at twice that |
| Modes | full sphere, upper hemisphere (about twice the texels per frame), or 3 x 3 billboards |
| Geometry | one card of 8 triangles |
| Channels | base colour and normal usually; a packed mask for roughness, opacity, depth |
| Blending | the 3 nearest frames on one card, by barycentric weights on the octahedral grid |
| Parallax | none, one depth read a frame, or iterative |
| Depth offset | possible, but "can make TAA look smeary" |

- Others: 16 x 16 in 1024 (a Switch game with Amplify Impostors, a Godot plugin, a three.js engine).
- Horizon used flat billboards (12 triangles), not octahedral impostors.
- **Billboard clouds** (Simplygon): leaves baked into a set of cards, the trunk reduced normally; plain triangle reduction thins foliage ("leaves generally have low connectivity").

**What it means here:**

- Hemi-octahedral, 12 x 12 or 8 x 8 frames a tree shape, albedo (coverage-keeping mips), normal and depth, baked at start-up on the GPU from the same meshes.
- Three frames blended, one depth read of parallax; no depth offset, which would also break the main pass's EQUAL test against the prepass unless both write it.
- The jungle patch is a few hundred metres across, so impostors matter for the valley's far trees later (kilometres), less for the patch itself.

## Small triangles and cut-outs

- GPUs shade 2 x 2 pixel quads: a one-pixel triangle pays four lanes. Merging quads cut shading of pixel-sized triangles 8 times (Fatahalian et al., SIGGRAPH 2010), the waste ordinary hardware pays.
- Long thin triangles do worst (Persson, "Triangulation").
- Nanite: a compute rasteriser for small triangles about 3 times faster than the hardware's, which takes over at about 32 pixels **(secondary sources citing Karis, SIGGRAPH 2021)**.
- Nanite Foliage (Unreal 5.7, experimental): branches as instanced parts, far clusters as 4 x 4 x 4 voxel bricks; Epic's reasons: cut-out cards mean heavy overdraw and flat look, and break simplifying distant pieces. Far beyond this engine for now.
- A discard in the depth prepass turns off early depth for it, so overlapping cards each run their alpha fetch: the prepass's cost is cut-out overdraw. Trimmed card outlines (done in `understorey-shadow-cascades` for the shadows) and fewer cards far away both cut it.

## meshoptimizer on foliage

Vendored v0.22:

- `meshopt_simplify` follows the topology and keeps seams and borders; the error is relative to the mesh's size (times `meshopt_simplifyScale` for metres). It stops early on many seams; weld first.
- `meshopt_simplifyWithAttributes`: weights about 1 for normals, 10-100 for UVs.
- Later versions add prune (small pieces), permissive (across UV seams) and clamped errors for selection (0.24-1.3); not needed to start.

**Good for:** the scanned plants (connected stems and leaves), bark if it were not generated.

**Bad for leaf cards:** each card is a separate quad of border edges, so the simplifier cannot reduce it short of deleting it; `meshopt_simplifySloppy` fuses leaves into blobs and smears their UVs. Cards are thinned by the generator instead.

## Ranked for this engine

1. **Levels for the trees and plants**, picked per view by error in pixels: bark from the generator, cards thinned and grown, scans through meshoptimizer. The main pass's geometry, the prepass and the probe capture all gain; most of the probe's 3.3 ms should go, since its faces are small.
2. **Shadows from coarser levels**, chosen by the camera's distance (the shadow leaves already are a coarse level; the bark could follow).
3. **Dithered fade at the big steps only**, a hashed pattern turned per frame for TAA and DLSS.
4. **Impostors** for the valley's far trees, when the trees spread past the patch.
5. **Vertex morphing** and anything Nanite-like: not now.
