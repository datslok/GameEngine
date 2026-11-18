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
