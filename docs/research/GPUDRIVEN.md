# GPU-driven drawing

Research for phase 18's GPU-driven item (the user, Nov 18 2026: fill the whole valley, build larger worlds): the GPU culls, chooses levels and batches every view into indirect draws; the CPU handles only coarse regions and changes. Plan in `ROADMAP.md`. Marked **[mem]**: from memory, not checked against a source; **[inference]**: our reasoning.

## Why: the CPU's per-draw work (baseline, `camera-timing`)

Laptop, release, hidden copy, ms. ~50K draws in the forest.

| View | Drawn | FRUSTUM | LEVELS | BATCH | MOVING | Camera | SHADOWS | CAPTURES | PACK |
|---|---|---|---|---|---|---|---|---|---|
| Jungle | 5,132 | 1.22 | 0.26 | 0.26 | 0.59 | 2.3 | 2.9 | 1.5 | 3.0 |
| Rim | 35,444 | 2.44 | 1.43 | 2.10 | 2.99 | 9.0 | 2.1 | 1.0 | 2.9 |
| Ring | 30,692 | 2.78 | 1.55 | 2.98 | 4.03 | 11.3 | 3.1 | 1.4 | 3.1 |
| Wall | 608 | 1.56 | 0.05 | 0.07 | 0.07 | 1.8 | 3.1 | 2.0 | 3.0 |

- FRUSTUM walks every draw (1.2-2.8 ms even when 600 are drawn); the rest grows with the draws in view; MOVING (the sway test and a second batch for the velocity pass) is the largest.
- With shadows, captures and packing, 8-19 ms of the CPU's frame is per-draw work, all of it growing with the world.

## Sources

| Source | What they do | Numbers |
|---|---|---|
| Vale, "Decima Engine: Visibility in Horizon Zero Dawn", GCAP 2017 ([slides](https://www.guerrilla-games.com/media/News/Files/GCAP2017_DecimaVisibility.pdf)) | Static world in tiles; instances sorted into clusters (filter, level range, Morton order); the CPU tests tiles and clusters, then one compute job per visible cluster: levels, frustum, size, occlusion (last frame's depth, camera only); a wave's output with one atomic and a prefix sum | 500K-1.5M static instances, 10-30 MB, 1-2 ms query; batching cut render items 60-70% |
| Haar & Aaltonen, "GPU-Driven Rendering Pipelines", SIGGRAPH 2015 ([slides](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)) | AC Unity: CPU quadtree, persistent instance data, draws merged by material/state hash, GPU instance and cluster culling, multi-draw indirect. RedLynx: everything on the GPU, two-phase occlusion, levels in the culling pass | AC Unity: CPU ~75% of before with ~10x the objects. RedLynx: 250K objects, culling 0.28 + levels 0.26 ms GPU, 0.2 ms CPU |
| Kapoulkine, niagara ([repo](https://github.com/zeux/niagara)) | A thread per draw: frustum, level by error over distance, late-pass Hi-Z, a visibility flag per draw; one shared vertex and index buffer; `vkCmdDrawIndexedIndirectCount`; output by atomics (order varies) | No published numbers |
| Wihlidal, "Optimizing the Graphics Pipeline with Compute", GDC 2016 | Cluster and triangle culling in compute, compaction by wave ballot and prefix | GPU triangle savings |
| Unreal 5 GPU Scene ([API](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Renderer/FInstanceCullingContext), [forum](https://forums.unrealengine.com/t/gpu-driven-culling-for-non-nanite-geometry/2717668)) | Persistent primitive and instance buffers, cached draw commands, a compute pass per view writing instance ids and indirect arguments | Epic: since 5.6 non-Nanite foliage is culled on the CPU again, "no plans" for non-Nanite GPU-driven: measure before going far |

- **Fixed order:** atomics per thread vary frame to frame; one atomic per wave is fixed only inside the wave; a fixed order needs prefix sums over a list fixed beforehand, or each instance writing at its own precomputed rank.
- **Vulkan:** indirect counts need `drawIndirectCount`; a non-zero `firstInstance` in indirect commands needs `drawIndirectFirstInstance`; `gl_InstanceIndex` includes `firstInstance`, so `drawList[gl_InstanceIndex]` keeps working **[mem]**.

## What this means for the engine **[inference]**

| Part | Today | GPU-driven |
|---|---|---|
| Instances | `GpuInstance` 208 B, still ones kept (`StillInstances`) | Plus a lean culling record (sphere, flags, level table index) kept between frames, written where changed (absorbs "Lasting GPU buffers") |
| Groups | `batchByMesh` sorts each view each frame | Groups per (style, mesh) built when still draws change; each instance writes at its fixed rank in its level's region: `batchByMesh`'s order, no sort |
| Levels | `chooseLevel` on the CPU per view | `level_of_detail.glsl` mirrored by `scene/level_of_detail`; a per-mesh level table; thinned bit set on the GPU |
| Culling | `scene/culling` functions | `culling.glsl` mirrored, the mirror's extra plane, light reach, smallest caster, cascade limit |
| Drawing | `vkCmdDrawIndexed` per batch | First `vkCmdDrawIndexedIndirect` per batch with GPU counts (shaders unchanged); later a shared mesh buffer and one multi-draw per pipeline state |
| Shadows | Casters scanned and hashed per tile (`shadowCasterSignature`) | Tiles as views; the cache invalidated by announced still changes touching a tile and by movers in reach (as virtual shadow maps do **[mem]**) |
| Captures, velocity | CPU scans per face; CPU walk for movers | Views; the velocity list written by the culling pass (`drawSways` mirrored) |
| Glass, mirror | Sorted and chosen on the CPU | Stay: few draws, precomputed lists |
| F4 counts | This frame, from CPU lists | GPU counters read back a frame or two late (as HIDDEN) |
| Ray tracing's TLAS | Per draw on the CPU | Out of scope (off by default) |

- **Expected:** the camera's CPU work from proportional to draws to proportional to groups (a few thousand commands): 9-11 ms to under 1 ms at the rim and Ring; GPU about 0.1-0.3 ms for ~50K instances over ~15 views.
- **Gain only where the CPU limits:** headroom elsewhere, and the condition for a fuller valley.

## Risks

- Draws exactly at a level switch may choose differently on the GPU (rounding): comparisons must allow it.
- A missed shadow cache invalidation leaves a stale shadow: the new rule must redraw wherever today's signature would.
- Two paths while the CPU one stays as the reference.
- Empty indirect draws still cost the command processor until the shared mesh buffer.
- Above ~500K instances, chunks of instances culled first (Decima's clusters) and coarse CPU cells become necessary.

## Stage 5: shadow tiles (`gpu-cull-shadows`), gathered Nov 18 2026

**The problem:** a tile is redrawn only when its fingerprint (`shadowCasterSignature`: every caster's mesh, matrix, shape and level) differs from the last. Computing the fingerprint needs the CPU's caster list, so every tile walks every draw each frame, cached or not: SHADOWS 2.1-3.9 ms at the rim, Ring and wall, nearly all of it for tiles then skipped (`TILES DRAWN 2 CACHED 3`).

| Source | What they do | Numbers |
|---|---|---|
| Haar & Aaltonen, SIGGRAPH 2015 (above) | AC Unity culls each cascade's casters on the GPU, also against an occlusion depth for the cascade made by reprojecting the camera's depth | 30-80% of shadow triangles culled ([DSOGaming's account](https://www.dsogaming.com/?p=80038), not checked against the slides) |
| Unreal 5 virtual shadow maps ([`EShadowCacheInvalidationBehavior`](https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/Engine/EShadowCacheInvalidationBehavior)) | Cached pages invalidated by what the scene reports: a primitive's transform changed, or its material moves vertices (world position offset); a primitive can opt out (Rigid, Static) | Forum reports of swaying foliage invalidating every frame ([1](https://forums.unrealengine.com/t/wpo-causes-shadow-glitches-when-rendering-with-vsm-using-caching-default/2707254), [2](https://forums.unrealengine.com/t/constant-invalidation-of-vsm-dynamic-shadows-in-5-7-0/2686534)) |
| Cyberpunk 2077, SIGGRAPH 2021 (`SHADOWCACHE.md`) | A budget of shadow updates a frame | |

**What it means here:** the cache's decision moves from a fingerprint of the casters to the changes the scene already announces, so the CPU's work follows what changed, not what exists; the GPU culls only the tiles redrawn.

| A tile is redrawn when | Known from |
|---|---|
| Its view changed (matrix, square, reach, smallest caster, cascade) or a shadow setting changed | the plan, as today (`ShadowTileMemory`, its key widened by the settings) |
| A still draw rebuilt in place touches it (old or new sphere in reach and view) | `FrameDescription::builtStillDraws`, the kept records' old spheres |
| The still draws were rebuilt whole (version not following) | that frame only, today's fingerprint on the CPU (exact, one frame at today's cost) |
| A moving draw that moved, or whose mesh deformed, touches it | the draws after `stillDraws` (hundreds), tested on the CPU |
| Something swaying may be in it (the nearest cascade with sway on) | redrawn every frame (today: whenever a caster in it sways, nearly always in the forest) |
| Glass tinting it changed | glass's cached list, on the CPU as today |

- **Tiles as views:** each redrawn tile a view of the culling passes: its own candidate test (casts shadows, shadow-only kept, hidden dropped, reach sphere, frustum, smallest caster, cascade limit; a new `culling.glsl` function mirrored by `prepareShadowDraws`' steps), levels by its texels (`tileLevelView`), its second list the movers (the far tile keeps them apart).
- **Room:** a view's working buffers (12 B a candidate) are scratch, reusable for the next batch of views; its draw list region (4 B a draw) and commands live to the draw.
- **Then the CPU path goes:** the renderer draws only the GPU's lists; `cull-compare` still builds the CPU's on request, as the reference.

## Stage 7: chunks and cells (`scene-chunks`), gathered Nov 18 2026

**Where it starts (after 5b):** GPU culling 0.1-0.2 ms for every view (~50K draws); PACK 2.8-2.95 ms, nearly all of it `writeScene` copying every instance, material and culling record into the frame's upload memory (~8 MB, ROADMAP's "lasting GPU buffers"); frames GPU-bound (the CPU waits 5.6-13.7 ms).

### Two-level culling

| Source | Hierarchy | Numbers |
|---|---|---|
| Haar & Aaltonen, SIGGRAPH 2015 (above) | AC Unity: CPU quadtree, GPU instances, chunks expanded to clusters (~64 of 64 triangles), clusters, multi-draw. RedLynx: flat GPU object culling and levels, two-phase | RedLynx, Xbox One: 250K moving objects, culling + levels 0.28 + 0.26 ms, CPU 0.2 ms **[M]**. AC Unity: ~10x the objects at ~75% of the CPU (claimed) |
| Vale, Decima visibility, GCAP 2017 (above) | Static tiles (one a streaming group, not a fixed grid); instances sorted by filter, level range and Morton code into clusters; the CPU tests tiles and clusters, a compute job per visible cluster tests its instances | 500K-1.5M instances, 10-30 MB, 1-2 ms **[M]**. Tiles immutable (a change rebuilds one, small changes to an "orphan" tile), movers in a k-d tree |
| van Muijden, "GPU-based procedural placement in Horizon Zero Dawn", GDC 2017 ([slides](https://www.slideshare.net/guerrillagames/gpubased-procedural-placement-in-horizon-zero-dawn)) | Placement blocks by footprint: 128 m for trees, 32 m for grass | 100K+ objects, ~250 µs GPU a frame **[M]** |
| Karis, "Nanite", SIGGRAPH 2021 ([slides](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)) | Flat instance culling per view, then a BVH8 over cluster groups walked by persistent threads | "A million instances easily" (claimed); persistent threads 25% faster than a dependent dispatch a level **[M]**, but rely on forward progress no API guarantees; 10M instances x 48 B = 457 MB: "instances are the new triangles" |
| Unreal 5.4+ scene culling (`r.SceneCulling`, [cvar wiki](https://indxzero.github.io/ue544cvarwiki/articles/r.sceneculling.mincellsize), excerpts only) | A loose hierarchical grid of cells holding chunks of instances, culled before the instances (Nanite only) | Smallest cell 4096 cm (~41 m); dynamic instanced objects uncullable (tested flat) by default; chunks of ~64 **[mem, unverified]** |
| Unity Entities Graphics ([chunks](https://docs.unity3d.com/Packages/com.unity.entities@1.3/api/Unity.Entities.EntityArchetype.ChunkCapacity.html)) | A chunk's bounds first; per-entity tests only in chunks partly in view | Chunks of 16 KB, at most 128 entities |
| Collin, "Culling the Battlefield", GDC 2011 ([slides](https://www.slideshare.net/DICEStudio/culling-the-battlefield-data-oriented-design-in-practice)) | Trees dropped for a uniform grid, static and dynamic grids apart, brute force inside visible cells | 15K spheres 0.6-1.6 ms on one console core **[M]** |
| Wihlidal, GDC 2016 (above) | Cluster culling | Draw count bottlenecks the command processor; 256 triangles a cluster the sweet spot on GCN (claimed) |

- **Flat culling is cheap:** 250K objects in 0.28 ms on an Xbox One; the hierarchy saves memory traffic and the repeat for each view, so it pays from ~1M instances or with many views **[inference]**.
- **Chunk size:** 64-128 instances everywhere it is given; a chunk of 64 is one subgroup. Per view: N/k chunk tests plus k for each chunk in or cut, best near k = 32-128 at 100K **[inference]**.
- **Grid over tree** for instances (Frostbite, Unreal's loose grid): constant-time insert, no rebuild. Chunks spatially tight (Morton order inside a cell).
- **Movers apart** in every source: a flat list tested one by one.
- **Levels:** tested per instance after the chunk (Decima); a chunk's nearest and farthest distance can skip whole chunks to a level.
- **Two levels need no persistent threads:** two dispatches (or a chunk's bit read first) lose one barrier.

### Cells and large worlds

| Source | Cells | Notes |
|---|---|---|
| Unreal 5 World Partition ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/world-partition-in-unreal-engine)) | Default 128 m cells, 256 m loading range (docs' example 256 m, 768 m) | A 2D grid; streaming sources with priorities; an HLOD per cell; a cap on cells loading at once |
| Marvel's Spider-Man ([interview](https://blog.playstation.com/2018/09/06/insomniac-interview-the-tech-behind-marvels-spider-man)) | ~800 tiles of 128 m | About a tile a second at top swing speed |
| Cyberpunk 2077 ([modding wiki](https://wiki.redmodding.org/cyberpunk-2077-modding/developers/file-formats/streaming-sectors)) | Sector grids by level: outside 64/128/256 | Coarser levels in bigger cells |
| Decima (above) | Tiles are the streaming groups | Loaded on a streaming thread, only switched on by the main thread |
| Star Citizen ([interview](https://gamersnexus.net/gg/2622-star-citizen-sean-tracy-64bit-engine-tech-edge-blending), [object containers](https://starcitizen.tools/Object_Container_Streaming)) | Object containers (a planet, a station), zones | 64-bit positions; the GPU stays 32-bit |
| Elite Dangerous ([80.lv](https://80.lv/articles/generating-the-universe-in-elite-dangerous/)) | Sector, layer, system and body packed in a 64-bit id | Doubles and paired floats |

- **Precision:** float steps ~4 mm 32-65 km out ([Godot](https://docs.godotengine.org/en/4.3/tutorials/physics/large_world_coordinates.html)); double ~1 mm at Neptune, ~1-2 m at a light year **[inference]**: a solar system fits doubles, a galaxy needs nested frames (Star Citizen's zones, Elite's sectors).
- **Camera-relative:** Unity HDRP about 50 km practical (transforms stay float); Cesium's relative-to-centre tiles (centre made eye-relative in double on the CPU).
- **Hysteresis and loads a frame:** no source gives numbers; common practice an unload radius 1.2-1.5x the load radius and a cap on loads a frame **[inference]**.

### What this means for the engine **[inference]**

- **Cells (CPU, ~128 m):** the unit of lasting GPU memory: each cell's instances and culling records in a range of GPU buffers kept between frames, written only when the cell is built or changes, so PACK's ~8 MB copy a frame becomes the changed cells' bytes. Later the unit of streaming (phase 13) and of HLOD; integer keys so a world past float's reach fits.
- **Chunks (GPU, ~64 draws, Morton order inside a cell):** a box each, tested once a view before the candidates; a candidate whose chunk is out stops at one read. Today's gain small (culling 0.1-0.2 ms); it keeps culling flat as instances and views grow.
- **Movers stay flat.**

