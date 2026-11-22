# Virtual geometry and virtual shadow maps: research

Phase 18 turned (Nov 21 2026): the forest's sun shadows popped all over from a stack of savings (cascades cached in steps, limited casters thinned, coarse stand-ins, cards), each patch exposing the next. The user wants AAA quality and is willing to rebuild the leaves as geometry. Sources marked [S] where stated, [I] where inferred.

## Why the cascades cannot simply be redrawn

Laptop RTX 4070, forest floor (59.5 3.6 -126.0), walking 1.5 m/s, GPU frame about 22 ms standing:

| Sun shadows | Walking | GPU frame |
|---|---|---|
| Cached in steps, thinned, stand-ins (`cascade-steps`) | 1.8-2.7 ms | 22.5 ms |
| Every cascade every frame, stand-ins everywhere | 15.3-15.8 ms | 36 ms |
| Every frame, seen meshes in the nearest two | 17-18.8 ms | 38.5 ms |
| Every frame, seen meshes in all four | 23-25 ms | 44.5 ms |

- Most of it is the 200 m cascade full of trees (about 14 ms a redraw). Cost grows with the forest, not with what is seen: against hard rule 11.
- Shipped forests with cascades [S]: Cyberpunk redraws up to 4 cascades every frame and amortises distant levels over frames (SIGGRAPH 2021); Insomniac and Call of Duty cache only still geometry, scrolled by texels every frame, and draw everything moving over a copy every frame; CryEngine caches far cascades and accepts frozen movers; Unreal fades shadows on the receiver over the last 10% of the distance; Days Gone and Ghost of Tsushima use screen-space shadows for grass and leaf detail. None thins casters at random as ours did.
- Popping causes and shipped fixes: swaying casters in cached maps (draw movers every frame; stop sway past a distance), cache moved in steps (scroll by texels), the shadow's end (fade on the receiver), seams (blend 0.1-0.3).

## Nanite-style virtual geometry

Karis et al., SIGGRAPH 2021 (advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf) [S]:

| Part | Design |
|---|---|
| Cluster | 128 triangles |
| Hierarchy (DAG) | Group 8-32 clusters by graph partition (fewest shared edges), merge, simplify to half with the group's border locked, split back into clusters; borders alternate level to level |
| Error | Quadric error projected to pixels; a group's clusters share one error and sphere; parent error forced at least the children's |
| Level cut | Draw a cluster where its parent's error is above the threshold (about 1 pixel) and its own at or below: local, no walk |
| Culling | A tree of groups walked by persistent threads (25% faster than a dispatch a level); two passes: last frame's depth pyramid, then the rejected retested against this frame's |
| Raster | Software rasteriser for clusters with edges under 32 px (3x the fastest hardware path); a 64-bit visibility buffer (depth, cluster, triangle); a material pass, under 2% overhead |
| Cost (PS5, about 1400p) | Culling and raster about 2.5 ms, materials about 2 ms; about 25M triangles drawn from over 1B |
| Streaming | 128 KB pages of clusters, hierarchy always resident, requested by error |

Open implementations [S]:

- meshoptimizer `clusterlod` (demo/clusterlod.h): groups of 16, half per level, borders locked, sloppy fallback, parent error the children's maximum, a hierarchy for traversal; built 1.64B triangles in under 3 min on a 7950X (zeux.io 2025).
- NVIDIA `vk_lod_clusters`: raw Vulkan on `clusterlod`, mesh shaders, compute raster for small clusters, streaming.
- Bevy (JMS55's posts 0.14-0.16): order meshlets and DAG, two-pass culling, visibility buffer, material pass; software raster cut its raster 3.44 to 0.42 ms; then culling was the cost; "DAG quality is the most important part, and hardest"; overflowing cluster ids and near-plane triangles were its bugs.
- Granite (Themaister, RTX 3070, 63.6M triangles): plain draws 5.5 ms, meshlet culling 3.9, mesh shaders with triangle culling 3.3, with tiny-triangle rejection 1.65-1.9; 32-64 vertex meshlets best on NVIDIA; hierarchical task shaders 10x slower.
- Weak cases: aggregates (foliage, hair) "not great": holes overdraw and do not scale with resolution; masked materials cost (Epic recommends geometry); wind by world-position offset inflates bounds (Epic moved to bones).

## Virtual shadow maps

Unreal's design (dev.epicgames.com, virtual shadow maps) [S]:

- 16k x 16k virtual maps of 128 x 128 pages; the sun a clipmap of 17 levels round the camera, each twice the last (64 cm to about 40 km); a page is made only where a pixel on screen needs it, at the level its size calls for (marked from the depth buffer).
- One physical page pool for all lights (512 to 8192 pages by quality; about 64 KB a page); levels follow the camera by whole pages with wrapped addressing, so pages in view keep their cache.
- Cached until something touching a page moves (by its bounds); world-position offset and skinning invalidate every frame; per object Rigid or Static ignores it (the shadow may not match the sway).
- Soft shadows by shadow-map ray tracing (8 rays of 8 steps, spread by the sun's size).
- Without clusters: "likely not a good fit": whole meshes are drawn into every page they touch.
- Foliage advice: stop sway past a distance, contact shadows alone for grass, distance-field shadows far.

Independent, closest to us: Sakmary's thesis (CTU 2025, C++ and Vulkan, RTX 4070 Ti SUPER; GPU Zen 3) [S]: wrapped page tables per level, meshlets culled in a task shader against a hierarchical page buffer; sun at 4096^2: marking 0.49-0.74 ms, drawing pages 0.18-0.30, sampling 0.30-0.59; with caching, 5-10x the triangles cost about the same, without it 5-40x slower; spikes from fast cameras and a small pool. Others: Stratus (OpenGL, atomics lose early depth), Blender EEVEE (all on the GPU, coarse levels first, pool overflow drops tiles).

Alternatives [S]: ray-traced sun shadows with opacity micromaps (Indiana Jones, RTX 5080, 1080p internal: sun tracing 0.71 ms; swaying foliage refits structures every frame; gigabytes of acceleration structures), one ray beat a virtual map in 2 of 3 of Sakmary's scenes but not with soft shadows.

## Leaves as geometry

- Epic moved from masked cards to modelled leaves [S]: "usually faster to avoid masked materials and instead rely on increasing triangle counts" (Fortnite Chapter 4); "masked-out pixels cost nearly as much as drawn pixels"; Nanite Foliage (5.7): leaves modelled one by one, trees 1-10M triangles, branches as instanced Assemblies (a tree 3.5 GB to 29 MB), voxels far, bones for wind with parts moving rigidly, animation stopped when small.
- Disjoint leaves under cluster simplification [S]: a leaf ends at one triangle, then vanishes, so canopies go bare; fixes: Preserve Area (survivors grow), voxels far, pruning (Cook et al. 2007: stochastic pruning, survivors scaled to keep area, contrast kept). [I] For us: groups formed by place, not shared vertices; prune by a fixed per-leaf rank and grow survivors (our card thinning moved into the hierarchy); clumps or voxels past a few pixels a leaf (`FARFOLIAGE.md`).
- Opaque leaves [I]: early depth and no alpha tests in prepass, shadows and rays; without a software rasteriser, pixel-sized triangles waste quads, so the main pass gain is uncertain until the visibility buffer.
- What our growers would need [I]: leaf prototypes of a few hundred triangles (outline from the alpha contour, a bend), twigs and branch units as instances, instance transforms per limb, a pivot a part for wind, a prune rank a leaf, trunk and leaf hierarchies apart.
- Our grass and floor cover already are what Epic moved to: opaque blades and leaves, levels keeping the cover (half the blades twice as wide, Ghost of Tsushima's); kept as they are. Their near cost is thin blades shaded in 2 x 2 pixel blocks, which a compute rasteriser addresses; swaying grass in virtual shadow pages would redraw them every frame, so Epic leaves grass to contact shadows.
- Wind on clusters [I]: a rigid transform per part from its limb's pivot keeps bounds exact; sway only near the camera, so far pages stay cached; cost grows with what moves near the camera.

## Building the hierarchy: when, and for which meshes

| Engine | When built | Which meshes |
|---|---|---|
| Unreal (Nanite, 5.9 docs) [S] | At import or when enabled in the mesh editor: "converting geometry to Nanite requires some processing time for each mesh", "significant time" for detailed meshes; a shared Derived Data Cache advised for many Nanite assets | A per-mesh "Enable Nanite"; "wherever possible", the gain largest for many triangles, small triangles, many instances, big occluders, virtual-shadow casters; a sky sphere the example left out; no morph targets, limited WPO (displacement clamped), opaque and masked only |
| Unreal, runtime [S, forum users] | Not in shipped builds: the builder module is editor-only; a 5.7 user reports a mesh built in under 200 ms and asks for it shipped (forums 2025) | Dynamic and procedural meshes converted to static meshes first, in the editor |
| Bevy [S] | `MeshletMesh::from_mesh` "very slow, meant to be ran once ahead of time, and not during runtime"; saved as `.meshlet_mesh` (5 MB loads in 12 ms) | Meshes converted on purpose |
| meshoptimizer `clusterlod` (zeux.io 2025) [S] | Offline: 1.64B triangles in 2 min 35 s on 16 cores (7950X), about 0.66M triangles a second a thread; 45-55 GB peak | All |

How `clusterlod` was made fast (zeux.io 2025) [S]:

- Parallel by mesh ("external": no shared data between threads), largest meshes first so one big mesh does not trail at the end; one mesh's levels stay on one thread (NVIDIA's `vk_lod_clusters` adds parallelism inside a mesh, for single huge meshes).
- Clusterizing was the largest cost (two thirds of the ray tracing build); fixes: O(vertex count) clears removed from the sparse paths (9 min 20 s to 3 min 31 s), a per-thread allocation cache (Windows heap contention: 4 min 20 s to 2 min 38 s), triangles sorted spatially first (`meshopt_spatialSortTriangles`, 5%).

Ours, measured (laptop, release, forest, nothing else running): 10.3M triangles in 2448 meshes; one level 5.6 s, every level 27.3 s (0.38M triangles a second, one thread, at load); start-up 32.8 s to 60.0 s. [I] Every engine above builds ahead of time and only meshes flagged for it; our forest is grown from seeds at start-up, so "ahead of time" here means a cache keyed by each mesh's bytes (the same seed and code give a hit, any change a miss, so nothing stale), and the build itself parallel by mesh, largest first.

## What it means here

- Shadows' cost would follow pages on screen, not the forest: the steps, thinning, stand-ins and cascade edges go; swaying pages near the camera still redraw every frame, so sway stops past a distance.
- The main pass (8-17 ms, triangle-bound) gains from cluster culling and levels before any shadow work; the card-to-3D pop goes with cards.
- Stage 4, virtual shadow maps for the sun: a clipmap of virtual maps in pages round the camera, pages marked from the depth buffer, one physical pool, clusters culled and drawn a page, pages kept until something in them moves; the cascades, their steps and thinning and the far map retired; then the lamps.
- Stages: `ROADMAP.md`, phase 21; stage 3's plan in `LEAFGEOMETRY.md`, stage 5's in `VISBUFFER.md`.
