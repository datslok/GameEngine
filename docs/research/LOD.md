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
- H was first the **render** height (under DLSS or TAAU, not the output's); since `lod-pop-in` the output's (Hiding the switches, below).
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

## Self-shadowed impostors

Researched on Nov 20, 2026, after small plants' cards 15-20 m away glowed yellow-green in sun flecks (gone with impostors off; dimmer with light through leaves off). The card casts no shadow and reads the scene's sun shadow once on its flat plane, so in a fleck every texel gets full sun plus light through the leaf.

| Source | What its impostors carry |
|---|---|
| [Unigine](https://developer.unigine.com/en/docs/latest/editor2/tools/impostors_creator/impostor_material) | depth and ambient occlusion textures; depth offset for shadows, or shadows and the image |
| [Pixyz for Unity](https://docs.unity3d.com/Packages/com.unity.pixyz.unity-sdk@4.0/manual/features/impostors.html) | depth captured to give impostors shadows |
| Amplify Impostors ([1.0 notes](https://reddit.birdcat.cafe/r/Unity3D/comments/1jui5td/major_update_for_amplify_impostors_long_time)) | occlusion and emission textures, an optional position output |
| [three.js](https://discourse.threejs.org/t/a-forest-of-octahedral-impostors/85735), [Meep](https://unpkg.com/@woosh/meep-engine@2.124.2/src/engine/graphics/impostors/octahedral/README.md) | depth beside colour and normal, for relighting and compositing |
| SpeedTree ([ambient lighting](https://docs.unity3d.com/speedtree-modeler/manual/ambient-lighting.html), [GPU Gems 3 ch. 4](https://developer.nvidia.com/gpugems/GPUGems3/gpugems3_ch04.html)) | ambient occlusion baked; billboards lit live, no baked lighting |

- No shipped write-up found of a card shadowing itself from the sun's direction; a [forum thread](https://redlib.hackliberty.org/r/GraphicsProgramming/comments/1l84vii/self_shadow_inpostor/mx2fexe/?context=3) advises the baked depth, not the card's, and 8 bits of it **(informal)**.
- **The idea used here:** the atlas already holds the plant seen from every direction over the upper hemisphere, so the frames round the sun's direction, with depth, are its own shadow map. The point seen is the card point moved along each view frame's direction to its baked depth; it is lit where no baked surface in the sun's frames lies nearer the sun, filtered over 2 x 2 texels, after a normal offset (Holbein, "Normal Offset Shadows", 2011) against acne where the sun grazes a leaf.
- The same point is where the scene's sun shadow is read, so a fleck's edge crosses the plant instead of switching the whole card.
- Rejected: baked occlusion alone (does not follow the sun; dims sun-facing leaves); a fixed dimmer (a tuning fudge).
- 8 bits of depth over the sphere's diameter is a step of 1/127 radius, below a frame texel (1/32 radius at 64 texels). It lives in the normal atlas's alpha, kept uncompressed (BC7 would blur it at silhouettes): 4 instead of 1 byte a texel.

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

## Terrain

Researched on Nov 15, 2026, after the camera's triangle count showed the ground's nearest ring (1280 m square at 2 m, 819 thousand triangles, one draw, also the collision mesh) as the largest single cost left.

| Design | How | Numbers | Source |
|---|---|---|---|
| Chunked LOD | a quadtree of pre-decimated meshes, cracks hidden by skirts | 4 m error drawn at about 2 pixels | Ulrich, SIGGRAPH 2002 |
| Unreal Landscape | components of 1 or 2 x 2 sections (7-255 quads a side); a section is the LOD unit, chosen by screen size, morphed in the vertex shader between heightmap mips | at most 1024 components; "each section is a draw call" | Epic's Landscape Technical Guide |
| Unity Terrain | Pixel Error: how far the mesh may stray, in pixels | default 5 **(unverified)** | Unity manual |
| Battlefield 3 | quadtree of 16 x 16 patches, heights sampled in the vertex shader, cracks stitched by index permutations | 2.5-3 ms of G-buffer on PS3 | Widmark, GDC 2012 |
| Far Cry 5 | 64 m sectors, 6 levels, 129^2 heights a node, 16 x 16 vertex patches stitched in the vertex shader, culled on the GPU | about 1.5-2 ms **(secondary source)** | Moore, GDC 2018 |
| CDLOD | one grid mesh instanced over the selected quadtree nodes, heights in the vertex shader; each level's range twice the last; vertices morph to the coarser level over the last 15-30% of a range, so no cracks and no pops | 7-9 levels, 105-446 thousand triangles, 0.08 ms of CPU selection | Strugar 2010 |
| Geometry clipmaps | nested grids following the camera, updated toroidally, blended over the outer tenth | triangles about 3-5 pixels | Losasso & Hoppe 2004; GPU Gems 2 ch. 2 |

- **Shadows:** CDLOD's advice is to choose the terrain's levels from the main camera and only cull with the shadow camera, so the shadowing surface is the seen one; a coarser shadow terrain leaks light on hilltops and darkens hollows by its error (Unreal users report it).
- **TAA:** a continuous morph moves vertices a sliver of a pixel a frame, which reprojection follows; swapping chunks pops depth and normals in one frame.
- **Density:** targets range from 2-pixel error (Ulrich) to 5-pixel triangles (GPU Gems); below about 2 pixels the rasteriser wastes its quads.

**What it means here:**

- The 2 m quad covers 15 pixels at 100 m and 2.4 at the ring's edge: too coarse by the path, too fine over most of the ring, and the jump to 16 m at 640 m is a visible step.
- **CDLOD fits best:** one instanced grid per view is this renderer's own model; the morph suits TAA and DLSS; choosing by 3D distance does not change as the camera turns; one scheme covers all 20 km. About 8 levels from 2 m (a quad about 8 pixels); an estimated 130-250 thousand triangles in view against 819 thousand plus the outer rings.
- **Its cost:** a terrain vertex path (heights from a texture, normals from differences at the level's spacing, the morph), mirrored on the CPU and tested like the other shader libraries; a CPU quadtree walk.
- **Collision and rays keep the mesh:** the 2 m ring stays as the `StaticLevel` (never drawn), and as the sky visibility bake's and ray tracing's stand-in; CDLOD's level 0 at the same 2 m points matches it where the player stands.
- **A first step with no new vertex path:** the near ring cut into 64 m chunks, each culled per view (about a third of the triangles in the camera's view, most chunks gone from the near cascades), which also become CDLOD's leaf nodes.

## Hiding the switches (pop-in)

Researched on Nov 17, 2026, after the user found the jumps between the trees' and plants' mesh levels "quite bad" (the impostor switches look fine). Several primary sources would not load (GDC Vault, Pixar's library, Epic's API pages); facts from secondary write-ups or memory are marked **(unverified)**.

### What switches today

Nothing fades: a draw takes its new level in one frame. The trees' steps, at pixel error 2 and a 70 degree view:

| Step | What changes | Error | Rendered 1080 px (DLAA) | Rendered 720 px (DLSS Quality) |
|---|---|---|---|---|
| Full → level | half the leaf cards, each grown to twice the area; bark at 30% | 0.41 m | about 160 m | about 107 m |
| → shadow stand-ins | a sixth of the cards at twice the size; bark at 10% | 1.0 m | about 385 m | about 257 m |
| → impostor | the baked card | 1.5 m | about 580 m | about 385 m |

- The levels are chosen by the **render** height, so DLSS or a smaller render scale pulls every switch about a third closer.
- Shadow tiles choose their own levels (orthographic, by the tile's texels), so a tree's shadow switches at other moments than the tree.
- The plants and palms step the same way (seen, coarser, shadow stand-in, card); the grass and floor cover halve, quarter, then end.

### Dithered cross-fades

Both levels drawn while fading, each keeping the pixels the other discards.

- **Unity** (built-in pipeline): `clip(fade - mask * sign(fade))` against a 4 x 4 screen mask, the two levels given the fade with opposite signs, so their pixels are exact complements: no holes, no doubled pixels. URP offers Bayer or blue noise.
- **Unity's timing:** distance-based (a share of each level's range; the manual's example 40%) or time-based (`LODGroup.crossFadeAnimationDuration`, global; 0.5 s by default **(unverified)**). Unity's caveat: time-based fades fade fewer objects at once, but show more when nothing moves.
- **Unreal:** "Dithered LOD Transition" per material (every material of the mesh, or it fails); time-based, `lod.TemporalLag` 0.5 s: two view origins half a second apart, each choosing a level, a mesh drawn at both where they differ **(mechanism unverified)**. A per-pixel hash rather than a Bayer matrix **(unverified)**.
- **Unreal's known problems:** foliage overdraw "significantly increased" while fading; a one-frame flash of the next level (UE-296133); instanced fades broken in 5.4 (UE-221257).
- **CryEngine 2** (Mittring 2007, via a summary): a screen-space dissolve **in the early-Z pass**, every started fade finishing "within a defined small amount of time"; the same dissolve fades distant objects out and hides the impostor switch.
- **Godot:** fades by alpha blending (the transparent path), and itself recommends dithered materials instead.

**With TAA and upscalers:** a pattern turned each frame is averaged into a soft blend by TAA, FSR and DLSS (Unreal's "DitherTemporalAA" exists for this); a fixed pattern stays a visible screen door. Weak history can flicker or ghost.

### Time, distance and hysteresis

- **Distance-based:** still when the camera is still, but an object standing in the band stays dithered for good.
- **Time-based:** every fade ends; a fade plays out while nothing moves, and only the few objects fading draw twice.
- **How long:** Giegl & Wimmer (below) found "below one second" removes popping; Unity's and Unreal's 0.5 s.
- **Hysteresis:** no engine publishes a number. Blender had a percentage, Godot margins in metres; about 10% of the switch metric is common practice **(unverified)**. Fast players need more than walking ones (DigitalRune).
- **Simplygon's rules of thumb:** each zone starting at twice the last one's distance, half the triangles a step, about 1 pixel of error.

### Shipped games

- **Horizon Zero Dawn** (Sanders, GDC 2018, a transcript): **shadow popping "almost completely solved"** by giving each object a fixed shadow level per cascade, by distance band, not following the camera's choice. Nothing found on its mesh fades.
- **Ghost of Tsushima grass** (GDC 2021, a summary, **unverified**): blades of 15 vertices near and 7 far, the vertices sliding so the near blade morphs into the far one at the switch; far away fewer blades about twice as wide; beyond, a texture.
- **Far Cry (2004):** hard switches everywhere (TU Wien's popping examples). Far Cry 5 and 6, The Witcher 3, RDR2, The Last of Us Part II: nothing primary found.
- **Nanite Foliage** (Unreal 5.7, experimental): no levels to switch; the error held under a pixel, far branches as voxel bricks. Far beyond this engine.

### Fading parts inside one mesh

- **SpeedTree's runtime:** one continuous level, 1 to -1. Each vertex stores its position and a *level position*, and the shader slides between them: small branches shrink onto their centre lines, and a share of the leaf cards "gradually shrunk away" while the rest grow, each card's own scale saying when. From 0 to -1 the tree fades into the billboard. Swapping unrelated meshes has "no smooth transition algorithm".
- **Stochastic simplification** (Cook, Halstead, Planck & Ryu, Pixar 2007, through Neubert's account): leaves drawn in a fixed random order, the last ones pruned as the screen size falls, the survivors' **area grown by 1 / kept share**, contrast corrected. That the pruned ones shrink over a narrow band rather than vanish is **(unverified)**.
- **Neubert, Pirk, Deussen & Dachsbacher 2011** (read): Cook's 1 / kept share is **too much for light pruning**. In their study (19 people, 5 trees) above 80% kept people preferred much smaller growth, the worst errors came at slight and moderate pruning, and the right growth differs by species (spruce barely needs any; beech more than 1 / kept share). This engine's leaf level is exactly Cook's rule at a half.
- **Geomorphing** (Hoppe's progressive meshes): needs each vertex's partner in the next level; meshoptimizer's collapses give one. Shading still switches in one frame.

### Unpopping (Giegl & Wimmer 2007)

- Over the transition t: up to a half, the old level opaque with depth writes and the new one blended over it at 2t; after, the new one opaque and the old one blended away. One level is always opaque, so depth stays valid.
- Artefacts: small silhouette mismatches, a jump in depth at the half, z-fighting (a small polygon offset).
- Not for here: its blended level needs a transparent pass, which cut-out leaves and the EQUAL prepass do not suit.

### What it means here

1. **Leaf cards fade one by one** (SpeedTree, Cook): each card a fixed random rank; as the error grows the cards past the kept share shrink to nothing over a short band and the rest grow smoothly, so the crown never jumps. One number a card and a few vertex instructions; no extra draws, the prepass untouched. Less growth than 1 / kept share (Neubert), tuned by eye. Fits the leaves, which are most of the jump.
2. **Dithered cross-fade, time-based, about 0.3-0.5 s,** for the bark and the step to the shadow stand-ins: both levels in the prepass and the main pass with complementary discards from one per-pixel noise turned each frame, so the EQUAL test still matches and TAA or DLSS blends it. Costs: fading draws drawn twice for half a second, bark on the cut-out pipeline meanwhile, each instance's fade in its record (`GpuInstance::hanging` has two free words).
   - **Built for the scanned plants distance-based, not timed** (`plant-fades`, the user's choice): with GPU culling a timed fade needs per-draw state kept between frames, a distance band is a pure function of the draw and the view. The prepass alone discards (the main pass follows by EQUAL); level-of-detail.md, Level fades.
3. **Hysteresis and steadier shadows,** nearly free: a dead band of about 10% so a level does not flick back, and shadow levels following the camera's choice by distance (or fixed per cascade, as Horizon) rather than each tile's own.
4. **Geomorphing the bark** (stored level positions from the simplifier's collapses), only if the dither's noise shows on trunks.
5. **Choose by the output height,** not the render height, so DLSS does not pull the switches closer; or keep it and lower the pixel error. A one-line change either way.
