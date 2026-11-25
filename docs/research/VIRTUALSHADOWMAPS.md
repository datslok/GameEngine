# Virtual shadow maps for the sun: research and plan

Phase 21 stage 4. The sun's cascades and far map replaced by a clipmap of virtual maps cut into pages, drawn only where the screen needs them and kept until something in them moves; leaves cast from their own clusters. Earlier notes: `VIRTUALGEOMETRY.md` (Unreal's headline design, Sakmary's numbers), `SHADOWCACHE.md` (still and moving casters). Sources marked [S] where stated, [I] where inferred, [M] where only remembered from Unreal's source and talks, not checked against a page.

## Today's bar (measured in 3f, `phase-21-2.md`)

- SHADOW 0.7-1.2 ms with the world still; 3.8-5.4 ms (frame +3-5.5 ms) with every cascade redrawn each frame (Wind `swayingShadowCascades` 4). Stage 4 has to beat the second, not the first.
- Texels today (70° 16:9, 4096 tiles): 0.22 cm at 3 m, 0.65 cm at 9 m, 2.5 cm at 36 m, 14 cm at 200 m, the far map's 2.7 m beyond (`sun-shadows.md`).
- Leaves cast through stand-ins; the cascades draw no clusters (old indexed path, `shadow.vert`), so 3f went here.

## How others do it

### Unreal (dev.epicgames.com, virtual shadow maps; Epic Games Japan 2023 tips; cvar defaults from the UE 5.4.4 source)

| Part | What [S] unless marked |
|---|---|
| Levels | 16k² virtual a level, 128² pages (a 128² page table); sun levels 6-22: 2^6 cm (64 cm) to 2^22 cm (about 40 km), 17 levels |
| Level choice | sun: by distance from the camera (turning re-marks nothing), `ResolutionLodBiasDirectional` -0.5 (scalability 0/0/-0.5/-1.5/-1.5; -1 doubles resolution); lamps: the pixel's footprint projected into the map |
| Coarse pages | some coarse levels marked without depth pixels, for fog and forward translucency (`MarkCoarsePagesDirectional`, `FirstCoarseLevel`/`LastCoarseLevel`) |
| Pool | one for all lights, `MaxPhysicalPages` 4096 (scalability 512-8192); 64 KB a page; with the static cache two slices: 4096 pages = 512 MB. Overflow: corrupted rectangles and a warning; since 5.4 the page level coarsens above 85% use (`DynamicRes.MaxPagePoolLoadFactor`) |
| Cache | pages kept until a caster's bounds (old and new) touch them; static and dynamic slices (`Cache.StaticSeparate` 1): a page with movers copies its static depth and draws the movers over; a mover still for 100 frames joins static [S, third party]; unused pages kept 60 frames [S, third party] |
| Per caster | Shadow Cache Invalidation Behavior: Auto, Always, Rigid (ignores its sway), Static (also ignores moves); "undefined" if a Static thing moves |
| Sway | world-position offset invalidates every page under its bounds each frame; fixes: WPO disable distance (per level: `Clipmap.WPODisableDistance`), Rigid/Static, contact shadows for grass; anecdotes: shadow depths 11 to 1.4 ms and 20 to 2 ms once foliage stopped invalidating |
| Nanite foliage (5.7, the Witcher 4 demo) | trees by bones, not WPO (tighter bounds, fixed-function raster); 100k bones 0.1 ms; skinning stops below `AnimationMinScreenSize` 0.1 (bind pose, no blend described); "VSM performance requires you to disable animation in the distance" |
| Drawing | Nanite clusters culled per level against a pyramid of page flags and an HZB of last frame's pages [M]; depth by atomics through the page table [M]; non-Nanite meshes "not a good fit": drawn into every page they touch |
| Filtering | shadow-map ray tracing (SMRT): 8 rays (7 in code) of 8 samples over the sun's angle, depth extrapolated behind occluders (max slope 5), a 0.015 screen-space ray first ("smart shadow bias"); noisy by design, left to TSR; normal bias 0.5/1000 × distance |
| Fortnite | uncachable cases "must fit into the performance budget"; `Cache.ForceInvalidateDirectional` drops the cache's bookkeeping for the sun; distant lamps one a frame |
| Costs | no Epic figures found; a third-party dense forest at 1080p: 2.1 ms on an RTX 4070 (0.3 allocation, 1.2 drawing, 0.6 sampling), 3.1 ms on PS5 [S, unverified] |

### Sakmary (CTU 2025 thesis; GPU Zen 3 ch. 12; code: Timberdoodle, github.com/Sunset-Flock/Timberdoodle) [S]

- **Table:** one R32 image a level (array layers): allocated, requested, failed, dirty, visible bits and a 16-bit physical place; 64² entries at most (built in one workgroup), so 4096² levels have 64² pages [I].
- **Pool:** one R32F image (16384² in tests) and a 64-bit meta image linking each physical page back to its entry.
- **Following the camera:** each level's light camera snaps by pages with wrapped (toroidal) addressing; each page stores the camera position it was drawn from, so a cached page is read with its own camera; a turning sun drops everything.
- **Marking:** per pixel, the footprint from depth and the normal's plane (four corner rays); level = max(log2(max(TW, TH)), 0); an atomic OR sets requested and visible, the first to set it appends a request; scalarised per warp. Grazing receivers pick too coarse a level.
- **Allocating:** a thread a physical page sorts free, used, cached (warp-aggregated appends); requests take free pages, then cached ones (no LRU); overflow sets failed, the page is not drawn; new pages cleared indirectly.
- **Moving things:** each moved object's box (old and new) projected into every level, its pages freed; one thread an object a level had outliers when a big object covered a whole level, fixed by splitting levels into sections. Skinned or swaying geometry not covered.
- **Drawing:** a 2×2 OR pyramid of dirty bits a level ("HPB", one 16×16 workgroup, as FidelityFX's single-pass downsampler); culling at the pyramid level where a box covers at most 4 cells: meshes, then meshlets in the task shader, then triangles (back-face and micro-triangle culling "critical"); every sun level in one indirect draw; depth by fragment atomics.
- **Filtering:** PCF, supersampled to 16 samples a pixel.

| Stage (RTX 4070 Ti SUPER, sun 4096², lamps 2048², cached) | Bistro | San Miguel | Sun Temple |
|---|---|---|---|
| Invalidate | 6.3 µs | 5.7 µs | 5.0 µs |
| Mark | 489 µs | 740 µs | 236 µs |
| Classify, allocate, clear, pyramid, expand | about 40 µs | about 40 µs | about 40 µs |
| Draw (sun) | 180 µs | 303 µs | 350 µs (overdraw under its roof) |
| Sample | 296 µs | 593 µs | 221 µs |
| All, lamps included | 2.61 ms | 2.81 ms | 1.15 ms |

- Sampling 1/2/4/8 samples: 296/563/619/928 µs (one traced ray: 2084 µs).
- Lessons: caching worth 5-10× once geometry is complex (5-40× slower without); spikes from fast cameras (wrapping plus disocclusion) and from returning to evicted areas; a small pool lowers the hit rate.

### Others [S]

| Who | Notes |
|---|---|
| Blender EEVEE Next (Foucault) | 256² pages, 32² tile maps (8192² virtual), 8192 pages at most; pages free, used or cached, the oldest cached freed first; "shadow buffer full" drops tiles; only a few levels update a frame (coarsest first). Drawing: atomics were slow at first; one view a tile through vertex-shader viewport and layer tried and dropped (up to 32×32 more instances, 64 views a redraw); one view a level with atomics kept. On Apple: depth in tile memory with the hardware test, copied into the atlas after |
| Frogfood (Jake Ryan, Vulkan) | 128² pages, 4096² levels, 10 levels over 25 km², a 4096² R32 pool; allocation one thread over a bitmask ("doesn't even register"); no mesh shaders: compute-culled meshlets, indirect draws, `imageAtomicMin`; PCSS; light-depth motion locked with a long frustum ("even with a 10 km frustum, 32-bit floats give sub-millimetre precision") |
| graphics-programming.org (Vulkan, RTX 3070, Bistro 4M triangles, 1080p) | marking plus allocation and drawing: 4096²/128² pages 2373 µs uncached, 388 µs cached; 2048²/64² 948 / 320 µs; marking widened by the filter's reach so filters never read an unmarked page; shading 314 µs without, 375/509/635/886 µs with 1/4/8/16 samples |
| StratusGFX (OpenGL) | 128² pages, 8K virtual; hardware PCF bleeds across page edges (fixes: a 1-texel border, filtering by hand at borders, or by hand everywhere); unallocated pages read the coarser level, the coarsest drawn whole each frame; fine pages a budget a frame (visible pop-in past it) |
| Olsson et al. 2015 (many lamps, GTX Titan, 1080p) | sparse cube maps; 256-356 lamps at 23-34 ms a frame; a still batch skipped when drawn last frame and no new or moving page overlaps it: about 40% fewer triangles |
| Treyarch sparse shadow trees (2016), Call of Duty IW (2017) | still sun depth stored (compressed quadtrees of 128² tiles; or a static D16 cache), moving things drawn over a copy each frame |
| Alan Wake 2 (REAC 2024) | clusters carry a separate shadow visibility set; nothing more public |

**Hardware sparse textures vs a page table:** every GPU-driven implementation uses a page table. Binding sparse pages is a CPU queue operation after a read-back: Olsson measured 7 µs a commit with spikes past 100 ms; NVIDIA's Vulkan `vkQueueBindSparse` has been measured at 0.25 ms for one page and 30 ms for 730 [S, forum].

## What it means here

The engine already has most of the parts:

- GPU culling a shadow view (`gpu-driven.md`), cluster culling with a pyramid test (`mesh-clusters.md`), still changes announced by the scene (old and new records), movers' records, a still copy with movers drawn over (the far map's `separateMovers`).
- Trees stop swaying below 0.1 of the screen, faded in to 0.15 (3c-iv-c, Unreal's own rule), decided by the camera for every view, so a tree and its shadow agree. Far trees are still, so their pages stay cached.
- Posed cluster bounds on the GPU (`MeshClusterPose`), so a swaying tree dirties only the pages its posed bounds touch.

### The design proposed

| Part | Choice | Why |
|---|---|---|
| Levels | 4096² virtual (32² pages of 128²), half-widths 0.5 m × 2^k, about 15 levels to 8 km | Sakmary's and Frogfood's size; a small table |
| Level choice | by distance from the camera: the finest level whose box holds the point, with a resolution bias setting | turning re-marks nothing (Unreal's sun); at 1440p and 70° the texel is 0.5-1 pixel (a pixel is d/1028; texels d/2048 to d/1024): at 3 m 0.15-0.3 cm, 36 m 1.8-3.5 cm, 200 m 10-20 cm, 1 km 0.5-1 m. A 4K screen wants 8192² levels (a setting) |
| Depth along the light | each level slides only across the light, snapped by pages; each level its own depth range, 16 half-widths either side plus the caster reach (200 m) sunwards, re-centred (its pages dropped) once the camera has moved 15 half-widths along the light (the finest every 7.5 m, a 128 m level every 1.9 km) | cached depth stays valid while a level slides (Frogfood); no per-page camera needed; one range of 16 km for all would make the finest level's depth steps 1 mm, nearly four of its texels (found in 4a); casters past the reach stay the far map's (distant occluders) |
| Table and pool | a page table a level (R32 array); a D32 pool image of 64² pages (4096, 256 MB), a setting | Unreal's default count |
| Allocation | on the GPU: requested pages take free, then the oldest unrequested cached ones; overflow coarsens the level bias next frame (Unreal 5.4) and lookups fall back to a coarser page | no CPU round trip, never a hole |
| Marking | a compute pass over the depth after the prepass, widened by the filter's reach; the mirror's picture marks its own pages | the screen decides; cost grows with pixels |
| Writing depth | the hardware depth test into the pool as a depth attachment: the task shader emits a cluster once for each dirty page it touches, the mesh shader places it there and clips it to the page with four clip distances [I] | everyone else uses fragment atomics, which lose early depth; our crowns are ~100 leaves a pixel deep. A cluster at its cut is a few texels to a page across, so most touch one page; EEVEE's failed per-tile views multiplied whole meshes, not clusters |
| Which clusters | culled a level against its box and a pyramid of dirty pages (Sakmary's HPB), twigs and far leaves through the same passes; the cut and twig choices the camera's | a caster must be the same in every level, or a seam shows where levels meet [S, Epic answer]; the camera's cut changes under a pixel, so it never redraws a page |
| Cache | invalidated by the scene's still changes and movers' bounds (old and new), by posed bounds for swaying trees; still and moving slices (Unreal's, our far map's): a page with movers copies its still depth and draws them over | cost grows with what changed |
| Swaying trees | dirty in the moving slice every frame they sway; a tree that starts or stops swaying redraws its still pages once | bounded by the 0.1 screen share ring |
| Soft shadows | today's PCSS (16 blocker taps, 16 filter taps, penumbra in metres) through the page table; taps clamped inside their page | keeps the approved look; SMRT is noisy without a temporal upscaler tuned for it |
| Bias | today's sun bias along the light, in the level's texels | already level-agnostic |
| Level seams | a hard switch | texels near a pixel make levels hard to tell apart (Stratus, Unreal) |
| Readers without depth | fog, probe captures, impostor cards: the far map kept as the coarse fallback | Unreal's coarse pages do this job; the far map is cached and keeps its movers apart already |
| GPUs without mesh shaders | today's cascades kept | every RTX 20+ and RX 6000+ GPU has mesh shaders |

What each cost grows with (hard rule 11):

- **Marking, sampling:** screen pixels.
- **Allocation, invalidation:** pool pages and changed casters.
- **Drawing:** dirty pages times the clusters in them.
- **Memory:** the pool (twice with the moving slice).

### What to expect

- **Still frame:** about today's 0.7-1.2 ms (Sakmary's sun: marking 0.24-0.74, drawing 0.18-0.35, sampling 0.22-0.59 ms). No gain there; the gains are elsewhere.
- **Swaying or turning:** only pages under swaying trees redraw, not four whole cascades; turning redraws nothing. Target: well under today's 3.8-5.4 ms.
- **Quality:** finer texels at every distance (1 m at 1 km against 2.7 m), no cascade edges, steps or thinning, leaves casting as drawn (stand-ins retired).
- **Memory:** pool 256 MB, 512 MB with the moving slice; the atlas shrinks from 384 MB to 128 MB once the cascades leave.

### Risks

| Risk | Mitigation |
|---|---|
| The swaying ring redraws many pages | measure at the four spots in 4e; coarse levels' moving pages redrawn every second or fourth frame (a lag, never a mismatch) |
| A fast camera allocates many pages at once | a pages-a-frame budget, coarsest first (EEVEE, Stratus); the coarser fallback meanwhile |
| A turning sun drops everything | sun direction in steps and levels taking turns, as `cascadeStagger` does today |
| Clip-distance drawing fails or is slow | fall back to fragment atomics into an R32 pool, as everyone else does |
| Grazing receivers read too coarse | distance-based choice avoids Sakmary's grazing failure |

## Stages

| Step | What | Checked by |
|---|---|---|
| 4a | Level boxes, page snapping, wrapped addresses, level choice, depth range: CPU maths mirrored in a `.glsl` library | tests: a point's level holds it, a slide keeps cached pages' addresses, the choice matches texel targets |
| 4b | Marking, page table, allocation, overflow on the GPU; F4 counts pages; a view colouring each pixel by level and page | the user checks the view by eye; counts at the four spots |
| 4c | Clusters drawn into pages (clip distances), every page redrawn every frame; the main pass can read it (a switch, the cascades kept) | the user compares both by eye; timings |
| 4d | PCSS through the page table, the coarser fallback, the far map behind | soft edges as today's; tests for the lookups |
| 4e | The cache: invalidation, still and moving slices, swaying trees | nothing redrawn when still; the four spots measured |
| 4f | Leaves casting from their twigs and far records; shadow stand-ins retired | by eye; timings |
| 4g | Mirror, probe, cards and fog moved over; cascades kept only for GPUs without mesh shaders | golden views; the user checks by eye |
| 4h | Lamps' pages (spots, point faces), after their own research | later |

### 4c in four steps (planned Nov 24 2026, after reading the culling code)

| Step | What | Cost grows with |
|---|---|---|
| 4c-i | The sun's casters: one more shadow view in the camera's culling and the placed plants', its box the level holding the farthest surface seen (the depth fit's measure, else the far plane) plus the caster reach sunwards; its clusters through a second run of the cluster passes (no occlusion, no twigs), cut by the camera's eye and pixel error, never by facing (a shadow view); F4 counts them | draws in the box, their clusters |
| 4c-ii | An expanding pass: each survivor's sphere (posed, or its draw's bounds when kept whole) into every level, each allocated page it touches listed (pair, page); a mesh shader a listed pair, its vertices placed by the vertex body (position only), then into the page's place in a D32 pool image, four clip distances at the page's edges; depth test GREATER, depth clamped (as every pipeline); cut-outs alpha-tested. The whole pool cleared each frame (every page redrawn: 4e caches) | survivors x levels touched; page pairs' triangles |
| 4c-iii | The main pass: a point's level, page, entry, physical page and texel, compared with the bias along the light in the level's texels (the cascades' `sunShadowBias`); no page there: the cascades as today. `virtualShadows.mainPass` (off) | screen pixels |
| 4c-iv | The fallback: an R32 storage image, `imageAtomicMax` of reversed depth in a fragment shader without a depth target; `virtualShadows.drawing` picks; both timed at the four spots against 3.8-5.4 ms | as 4c-ii |

- Not drawn into pages before 4f: twigs and far leaves (leaves cast nothing there yet), and draws outside the pools (deforming meshes, cloth), which stay the cascades' until 4g.
- Reused, not copied: `VulkanMeshClusterCulling` takes a view's command and placed-command offsets (today the camera's, view 0); `meshClusterVisibility` skips the facing test for shadow views (mirrored).

## Swaying clusters' bounds (researched Nov 25 2026, after 4c-v)

4c-v found draws kept whole 54-69% of the pages' triangles: `meshClusterSeenWhole` keeps every cluster of a draw whose shape is not its mesh's (terrain patches, impostor cards, ground-following, lianas, thinned cards) or that sways per vertex without bones, and the expanding pass lays the draw's whole bounds into each level for each of its clusters.

- **Unreal:** a material's Max World Position Offset Displacement caps how far its vertex animation moves a vertex; Nanite's clusters "each have their own individual bounds and are culled individually on the GPU. Clamping WPO is a good way to manage this" ([Nanite content](https://dev.epicgames.com/documentation/en-us/unreal-engine/working-with-naniteenabled-content)). Too small: parts culled wrongly ([UE-247597](https://issues.unrealengine.com/issue/UE-247597)); too large: more culled in and more pages invalidated.
- **Unreal's cache:** WPO geometry invalidates its cached pages every frame; per primitive Shadow Cache Invalidation Behavior (Auto, Always, Rigid: WPO ignored, Static) and a WPO Disable Distance limit it ([VSM docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine)); bounds too large invalidate more pages than needed. Forum reports: WPO foliage the usual VSM cost ([VSM with Nanite foliage](https://forums.unrealengine.com/t/vsm-with-nanite-foliage-wpo/776406), [ShadowDepth with foliage](https://forums.unrealengine.com/t/shadowdepth-tanking-fps-with-nanite-vsm-foliage-ue-5-3/2072806)).
- **Here, no number to tune:** `windSwayOffset`'s every term is the spring's lean times a length fixed by the vertex, and the lean is softly held under `WIND_MOST_LEAN` (0.5) times the fade. For a cluster's vertices, height share at most s = (top of its sphere) / the sway's height, trunk distance at most d: the bend moves a vertex at most 2 |lean| H s^2 (its radial projection back to its length is within twice the sideways step), the branches' bob `WIND_BRANCH_SHARE` |lean| d min(s, 1), the flutter sqrt(3) |flutter| |lean| min(s, 1). A cluster near the foot barely moves; the reach is exact maths, not an artist's cap.
- **Cost:** a few operations a cluster where the draw sways (in the cluster passes and the expanding pass); it replaces a draw's every cluster in every page of its bounds with each cluster's own pages. The camera's passes gain too: today they keep every cluster of a swaying plant near the eye.
- **Counted (4c-vi's first step):** of the whole draws' triangles, lianas 3.7-42.2 M (most at the wall and among the trees: a liana's bounds span its tree, so each of its clusters lands in 100-180 pages), swaying without bones 10.2-22.6 M, terrain patches 5.6-7.6 M; cards, ground-following and thinned none. Lianas and terrain patches need bounds of their own, not this reach.
- **Later, by the same logic:** 4e's caching wants the same reach (a swaying cluster's pages redrawn, a still one's kept), and a disable distance (Unreal's WPO Disable Distance, our `swayFadeEnd`) already stops sway far off.
- **Decided (the user, Nov 25 2026): the worst lean first.** `WIND_MOST_LEAN` times nothing else, not this frame's lean: the cluster passes read no wind springs (only bones' leans), and grass reads a spring a blade, so one lean a cluster bounds nothing. The fade is not taken either (`GpuCameraCull` has no room for its start). The bob's distance is across from the trunk, as `windSwayOffset` takes it. This frame's lean only if the measured gain is worth binding the springs to four passes.
- **Measured: little to gain here.** The swaying draws kept whole were grass and small plants at 0.7 pages a cluster, and a cluster takes at least one, so even an exact bound saves little; the worst lean grew small plants' clusters past their plants, so the smaller of the two is taken. The pages came out even, the camera's cluster triangles at the wall 21% fewer. This frame's lean is not worth binding the springs. Lianas (100-180 pages a cluster) are where own bounds pay.

## Lianas' bounds (researched Nov 25 2026, after 4c-vi)

A liana's draw is one template tube (or ivy cards) bent in the vertex shader onto its record's catenary (`liana.glsl`), its holds moved with their trees' bark, swung by its arm below the chord. `meshClusterShapeFixed` fails for it, so every cluster is kept whole and laid into the pages by the draw's bounds (`lianaPose`: the whole liana's box), 100-180 pages a cluster, 3.7-42.2 M of the pages' triangles.

- **Unreal:** bends spline meshes in Nanite's vertex stage (experimental in 5.3; [Nanite content](https://dev.epicgames.com/documentation/en-us/unreal-engine/working-with-naniteenabled-content)); from memory of the source, not confirmed by a page: a cluster's bounds bent by the spline over the cluster's range along the mesh's forward axis, read from slices of the spline. WPO beyond that is covered by Max World Position Offset Displacement, as in 4c-vi. Bug reports of parts culled wrongly ([UE-247597](https://issues.unrealengine.com/issue/UE-247597)) show the price of a bound too small.
- **The template's x is the share along it**, untouched by the model matrix: a cluster's vertices with x in [s0, s1] lie on the arc between shares s0 and s1. Its length is l = (s1 - s0) L, so every point P of it has |PA| + |PB| <= l (A, B the arc's ends): the arc lies in the ellipsoid of foci A, B, within the sphere at their midpoint of radius l / 2. Exact for any curve, with no maths of the catenary's own; two `lianaCentre` calls a cluster.
- **A cluster's [s0, s1] from its sphere alone:** every tube vertex lies on the unit cylinder (y^2 + z^2 = 1; an ivy corner at sqrt 2), so a vertex v in a sphere (c, r) has |v.x - c.x| <= sqrt(r^2 - (1 - |c.yz|)^2). Simplified levels keep the mesh's vertices (meshoptimizer collapses to existing ones), so this holds at every level. Testable over every cluster of the templates, without a new buffer.
- **Grown by:** the tube's radius (`from.w`); the ivy's grown card (`lianaLeafPose`'s reach); the holds' motion, this frame's `lianaAnchorMotion` blended by share (the end points' offsets: the arc's sphere moved by their mean, grown by half their difference); the swing, |lean| < `WIND_MOST_LEAN` (`windFadedLean` is softly held), so a vertex of arm a moves at most a sqrt(lambda^2 + (1 - sqrt(1 - lambda^2))^2), 0.52 a at lambda 0.5, plus the hanging flutter's reach; arm at most min(curve.z, the ends' arms + l / 2), so the clusters at the holds barely grow.
- **Ivy:** a card cluster's x maps to cards [c0, c1]; `lianaLeafPlace` is monotone in the card (its jitter 0.6 under one card), so its stems lie between shares `lianaLeafPlace(c0, 0)` and `(c1, 1)`.
- **Cost:** a record read, two centre points, a share range and a few square roots a liana cluster in the cluster passes and the expanding pass; the anchors' motion read once a frame where `liana_trees.comp` already finds each anchor's plant, or per cluster from the bones. It grows with the liana clusters culled, and replaces each cluster's 100-180 pages with its own few.

- **Decided (the user, Nov 25 2026):** spans from the clusters' own spheres (no table), the anchors' motions a pass after the bones; the template stretched (64 along) so clusters are runs of rings; draws' spheres by their farthest corner engine-wide.
- **Measured:** the corner sphere did most of it (lianas kept whole 0.6-15.6 M of the pages' triangles, from 3.7-42 M); the spans' bounds took them to 0.2-0.9 M, VSMDRAW 1.9 ms less at the wall, 0.4 among the trees, even elsewhere. The worst lean's swing is most of a swaying cluster's bound (5-9 m against 2-3 m still): this frame's lean only if a later measurement asks.

## Terrain clusters' bounds (researched Nov 25 2026, after 4c-vii)

A terrain patch (`scene/terrain`, CDLOD) is one flat grid mesh (32 x 32 quads, 2,048 triangles; the half grid 16 x 16) scaled over its node and lifted onto the land in the vertex shader (`terrainVertex`). `meshClusterShapeFixed` fails for it (`material.z` names the patch), so each of its clusters (124 triangles at most: about 8 x 8 quads, some 16 a full patch) is laid into the pages by the patch's whole sphere (`appendTerrainDraws`: the node's square and its lowest to highest height). In the forest a leaf is 64 m (32 squares of the finest 2 m spacing), so a nearest patch's sphere is 45 m or more across in every fine page it touches, for each of its 16 clusters. 5.6-7.6 M of the pages' triangles (4c-vi's count).

- **Unreal:** the Landscape was "one of the biggest initial offenders" for VSM in Fortnite Chapter 4; Epic added simple Nanite Landscape support "primarily to improve Virtual Shadow Map performance", which "effectively eliminated it as a problem": the landscape becomes clusters with bounds of their own. Non-Nanite geometry that is "physically large" or overlaps "many Virtual Shadow Map pages" is the cost ([Epic, VSM in Fortnite Ch. 4](https://www.unrealengine.com/en-US/tech-blog/virtual-shadow-maps-in-fortnite-battle-royale-chapter-4); [Nanite Landscapes](https://dev.epicgames.com/documentation/en-us/unreal-engine/using-nanite-with-landscapes-in-unreal-engine)). A Nanite landscape is a baked mesh, rebuilt when the land changes; ours stays a heightfield.
- **CDLOD (Strugar 2010; `LOD.md`):** a node's box from a min/max quadtree over the heights, built once; our `TerrainBounds` already is one, from 64 m leaves up. The same tree bounds any box: at the level whose node is at least the box's size, the box overlaps at most 2 x 2 nodes, so four reads give a range that holds every height inside it.
- **What a cluster's vertices can reach:** each grid point lands on the land at its node place, slid by the morph towards its even neighbour, at most one spacing along x and along z (`morphGridCoordinate`), and the height there is bilinear (or the finest drawn triangles) between grid points, which `TerrainBounds` and `terrainHeightRange` both cover. So a cluster's vertices lie in the box of its grid points grown by one spacing each side, between the lowest and highest heights over that grown box. Exact for every view and morph, no tuning number.
- **Far Cry 5** (GDC 2018, from memory, not checked against the slides): the terrain's quadtree walked on the GPU, its patches bounded by a min/max mip chain of the heightmap. Today's GPU-driven terrains bound on the GPU; CPU tables are CDLOD's older form.
- **A cluster's footprint:** its grid points' x and z in the mesh (0 to 1 across the patch), the same for every patch of that grid: its exact box, kept once a mesh in the cluster's spare words (its sphere's box is about 40% wider a side).
- **Expected:** a cluster in its own few pages, not its patch's 16 clusters' worth: the terrain's pages' triangles about 5 to 10 times fewer (5.6-7.6 M to about 1 M), VSMDRAW perhaps 0.5-1 ms less; small next to caching (4e). The camera's passes gain too: today every cluster of a patch in the frustum is kept, none culled by the pyramid.
- **Decided (the user, Nov 25 2026): the bounds on the GPU.** A min/max pyramid a height grid, each level's texel the lowest and highest of the 2 x 2 below, down to the grid's own spacing (about 2.7 times the heights' memory), built when the heights are uploaded. A cluster's box from the patch's corner and size, grown by one spacing; the finest grid holding it whole, its level where the box spans at most 2 x 2 texels, four reads; the sphere round that box. Mirrored in C++, a switch (`passes.meshClusterTerrainBounds`) to time it on and off. Not the CPU table first offered: its cost is the CPU's every frame for every patch, and GPU selection, terrain occlusion and changing land (phase 15) want the pyramid.
- **Cost grows with** the terrain clusters culled: four reads and a sphere each, a view; the pyramid's build with the heights, once.

## Decided (the user, Nov 24 2026)

1. Writing depth: clip distances with the hardware test tried first; fragment atomics as the fallback, both behind a switch in 4c and measured at the four spots. Not found elsewhere: Unreal writes atomics even for meshes without Nanite (its software rasteriser came first); EEVEE's per-tile views copied whole meshes, where a cluster mostly lands in one page; few shadow fragments stack on most scenes, our crowns about 100.
2. GPUs without mesh shaders keep today's cascades.
3. Fog, probes and cards read the far map, which also stands behind missing pages (after any coarser level); Unreal-style coarse pages would mark the fog's volume and the probe's every direction (a level is 1024 pages) for blurry answers. Revisit if the two systems or the jump to 2.7 m texels show.
