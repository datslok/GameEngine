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
| Soft shadows (reopened Nov 25 2026: SMRT, see the re-check) | today's PCSS (16 blocker taps, 16 filter taps, penumbra in metres) through the page table; taps clamped inside their page | keeps the approved look; SMRT is noisy without a temporal upscaler tuned for it |
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
| 4d | SMRT through the page table, the coarser fallback, the far map behind ("soft shadows through the pages") | penumbrae against the exact disc; the four spots measured |
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

## The design's picks re-checked (Nov 25 2026, before 4d)

Each pick of "The design proposed" and what was built since, against what current engines ship and what each cost grows with; a pick resting on reuse, an earlier note or ease is reopened.

| Pick | Current engines | Its reason | Verdict |
|---|---|---|---|
| Soft shadows: PCSS | Unreal: SMRT (rays over the sun's disc through the map) | "keeps the approved look"; "SMRT is noisy without a temporal upscaler", but TAA came since | **Reopened: SMRT** (4d) |
| Eviction: the GPU's order, not least recently used (built in 4b, `virtual-shadows.md`) | Unreal keeps unused pages 60 frames [S, third party]; EEVEE frees the oldest cached first; only Sakmary takes any, and found returning to evicted areas spiked | "as Sakmary's", simpler; this doc's own design said the oldest first | **Reopened: oldest first**, in 4e (a cache's hit rate is 4e's whole gain) |
| Overflow: the level bias coarsens next frame (Unreal 5.4, above 85% of the pool) | Unreal 5.4 | planned, never built: a full pool marks pages `FAILED` and the cascades shade them | **Still owed:** with 4d-ii's fallback, before 4g retires the cascades |
| Bias: the cascades' along the light, in the level's texels | Unreal: a normal bias of 0.5/1000 x distance and SMRT's short screen-space ray first ("smart shadow bias") | reuse of the cascades' | **Reopened with SMRT:** its rays start off the surface, so the bias is part of 4d's research |
| Readers without depth and the last fallback: the far map, no coarse pages | Unreal marks coarse levels for fog and translucency; Stratus draws its coarsest level whole | user's decision Nov 24 2026: a level is 1024 pages (a quarter of the pool) for blurry answers; the far map is already cached and holds movers apart | **Holds on merit** (cost grows with the pool); re-check at 4g with a failed page's look: the far map's 2.7 m texels against a coarse level's |
| The clusters' cut: the camera's, in every level | Unreal: Nanite picks a cut for the shadow view by its own texels [M, to confirm] | a caster the same in every level (no seams, Epic's answer [S]); the camera's cut changes under a pixel, so it never dirties a page | **Holds on merit** for now; confirm Unreal's choice in 4e's research, since a cut that moves with the camera could dirty cached pages |
| Clusters culled by a pyramid of dirty pages (Sakmary's HPB) | Unreal: a pyramid of page flags and an HZB of last frame's pages [M] | planned for 4e; 4c draws every page | **Holds:** 4e |
| Levels 4096² (32² pages), the choice by distance, depth ranges per level | Unreal 16k² levels by distance with a bias; Sakmary, Frogfood 4096² | texels 0.5-1 pixel at 1440p, 8192² a setting for 4K | **Holds on merit:** cost grows with screen pixels either way |
| Writing depth: atomics | Unreal: atomics | measured 24-39% faster than the depth test (4c-iv) | **Holds** (measured) |
| Swaying trees: the moving slice, still past 0.1 of the screen | Unreal: WPO disable distance; bones for Nanite foliage (5.7) | cost bounded by the swaying ring | **Holds** |
| Hard switch between levels | Unreal, Stratus: hard [M for Unreal] | texels near a pixel hide the switch | **Holds** |
| GPUs without mesh shaders keep the cascades | Unreal draws non-Nanite meshes into its pages by the ordinary raster | the user's decision Nov 24 2026: every RTX 20+ and RX 6000+ has mesh shaders | **Holds** (a scope choice, not a scaling one) |
| Leaves casting from their twigs (4f) | Unreal: Nanite foliage casts as drawn | as engines | **Holds** |

## Soft shadows through the pages (researched Nov 25 2026, before 4d)

4c-iii reads one texel of a point's page: hard edges. The cascades' soft edge (`softShadowFromTileLifted`, `sun-shadows.md`) is PCSS: 16 taps average the blockers' depth, the penumbra tan(sun's angular radius) x (receiver - blocker) in metres, clamped to 1-12 texels, 16 hardware-filtered comparisons over it. PCSS was first planned here too, then reopened ("the design's picks re-checked" above): Unreal ships shadow-map ray tracing (SMRT).

### How others do it

- **Unreal (UE 5.9 docs):** "shooting a number of rays towards the light source ... a number of samples along the ray are projected and tested against the virtual shadow map to achieve soft shadowing and contact hardening"; rays over the Source Angle; 8 rays (`SMRT.RayCountDirectional`; 0 "reverts to single-sample hard shadows"), 8 samples a ray (`SamplesPerRayDirectional`, "between 4 and 8 samples works well"); fewer rays "show visible noise in the penumbra"; PCF "would over-blur". The map holds only the first layer, so "light leaking artifacts can happen where the occluders overlap", fixed by "a gap-filling heuristic that extrapolates depths behind the first occluder based on depths seen before the point of occlusion", `ExtrapolateMaxSlopeDirectional` 5 (0 = none: "reduced penumbra quality"; higher "can introduce light leaks behind second occluders"). The penumbra is clamped (`MaxRayAngleFromLight`, `RayLengthScaleDirectional`) "to avoid sample divergence from the ray origin". An adaptive ray count makes softer shadows dearer; a forum post found it caused "pixel artefacts on moving shadows" ([VSM docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine); [cvar](https://unrealdirective.com/resources/console-variables/r-shadow-virtual-smrt-extrapolatemaxslopedirectional/); [forum](https://forums.unrealengine.com/t/virtual-shadow-maps-for-high-quality-rendering-cinematics/1922741)). Not public: how samples are spaced, the extrapolation's formula, the adaptive rule [M: a few rays first, the rest only where they disagree].
- **Cinnabar (open source, Vulkan, merged Nov 2026):** rays on a Vogel disc turned by blue noise; marched out from the pixel to 20 m, steps "spaced by the square of their share" (nearer together near the pixel), jittered per pixel; the last step at the ray's end, so taller casters still cast with the penumbra capped (about 9 cm for the sun); a step "blocked when it is behind the map's depth by more than two of the level's texels and 2% of its distance"; behind the map counts as solid (a heightfield, no extrapolation); every step reads the point's level through the page table, a missing page the next coarser level's; marking widened by the light's size times the ray's length. High 4 x 6, Ultra 8 x 8: 0.21 / 0.57 ms more than hard shadows on an RTX 5090 at 1080p, a 2° sun over casters filling the view; blue noise and TAA, no denoiser ([PR #421](https://github.com/aszecsei/cinnabar/pull/421), [issue #257](https://github.com/aszecsei/cinnabar/issues/257)).
- **MetalRenderer (open source, Metal):** "a shadow sample marches its jittered segment toward the light through the map"; a sample whose pages are missing traces the whole ray in hardware instead ([PR #31](https://github.com/IIMrFreemanII/MetalRenderer/pull/31)).
- **Older work:** backprojection (Guennebaud et al. 2006) integrates the map's texels over the light seen from the point; same first-layer gaps and overlaps ([paper](https://www.labri.fr/perso/guenneba/docs/SoftShadowMapping-egsr06.pdf)).
- **PCSS's own numbers** (graphics-programming.org, GPU Zen 3): 1/4/8/16 samples 61/195/321/572 µs over none at 1080p ([article](https://graphics-programming.org/blog/virtual-shadow-maps)).

### The physics, and why rays beat PCSS here

From a point, the sun is a disc of angular radius α (tan α = 0.00465). A ray towards a point u of the disc (|u| <= 1) climbs a height h along the light while moving h tan α u across it. In the light's frame a level is a straight map of (across, along), so a ray is a straight segment in a level's texels and depth: from the point's texel and depth, h tan α u / texel across and h x depth scale along. A sample is blocked when the stored surface there is sunward of the ray. The share of rays that get through is the share of the disc the point sees, each ray finding its own blocker: a canopy over a canopy, sun flecks through gaps, a twig over a leaf. PCSS's one averaged blocker height a pixel smears those.

### What it means here

- **Rays clamped to the marked pages, as Unreal clamps its penumbra:** a ray's sideways reach capped at `VIRTUAL_SHADOW_RAY_TEXELS` of the point's level (12, the cascades' widest penumbra: texels follow pixels, 0.5-1 a texel, so about 6-12 pixels), so its height H = 12 texels / tan α (the finest level 0.6 m, the 1 km level 1.3 km), and no higher than the caster reach (200 m) the levels' depth ranges hold sunwards: past it the far map's distant occluders as now. The last sample at the ray's end blocks for anything above it there, so taller casters cast, their penumbra capped (Cinnabar's rule). A wider cap marks more pages (by its square).
- **Pages:** every sample within 12 texels of its point, so all of a point's samples lie on at most 2 x 2 pages: their four entries read once a point, a sample's by which side of the page edges it falls. One texel a sample (`texelFetch`, no filtering): no gathers, no page-edge special case. The marking needs 12 texels, not 24 (one texel a sample, no 2 x 2 filter).
- **Samples along a ray:** spaced by the square of their share (more near the point, where contact shadows are sharp), jittered per pixel; rays on a Fibonacci (Vogel) disc turned per pixel by the interleaved gradient noise (`fibonacci.glsl`, the cascades' turn); TAA averages them.
- **Blocked:** stored depth sunward of the sample by more than the bias. The ray starts from the point lifted by the cascades' bias along the light in the level's texels (4c-iii's `virtualShadowBiasDepth`, measured acne-free), the samples' tolerance growing with their height as Cinnabar's (two texels and a share of the distance). Unreal adds a screen-space ray ("smart shadow bias"); ours exists already as contact shadows (`contact_shadows.glsl`).
- **Gap filling:** Unreal's extrapolation behind an occluder, slope capped (5), a setting with 0 the plain heightfield; its formula ours (not public), tested on two overlapping blockers.
- **Missing pages:** the point's own page missing, the next coarser drawn level's (levels share the light's axes: texels halve, depth by the level's scale and offset), then the cascades until 4g; a sample's page missing, the same chain for that sample. Coarser pages are drawn only where some pixel marked them; the far map stands behind (decided Nov 24 2026, point 3).
- **Overflow (owed since 4b):** a pool past `virtualShadows.poolLoad` (0.85, Unreal 5.4's) coarsens every level's choice one step next frame, back when it falls under; F4 shows the bias.
- **Distant occluders:** the far map's soft lookup as now.
- **Not in 4d:** glass's coloured shadows and impostor cards stay the cascades' (4g).

### Picks

| | SMRT (picked) | PCSS |
|---|---|---|
| Cost grows with | screen pixels on pages x rays x samples (64 reads at 8 x 8; fewer with the adaptive count) | screen pixels on pages (16 reads + 16 gathers) |
| Current engines | Unreal 5 (8 x 8, adaptive), Cinnabar | cascaded engines, Frogfood |
| Reason | each ray finds its own blocker: layered canopy right; no page-edge cases | none left: its reasons were reuse and an old note |

| | Clamp the ray in the point's level (picked) | Climb into coarser levels as the ray rises |
|---|---|---|
| Cost grows with | pages marked as today | pages marked: several coarser levels a pixel |
| Current engines | Unreal (`MaxRayAngleFromLight`, `RayLengthScaleDirectional`), Cinnabar (20 m) | none found |
| Reason | engines clamp; penumbrae as wide as today's | wider true penumbrae, unproven |

| | Extrapolation behind occluders, a setting (picked) | Heightfield only |
|---|---|---|
| Cost grows with | samples (a few operations each) | samples |
| Current engines | Unreal (max slope 5) | Cinnabar |
| Reason | fixes leaks where blockers overlap, common in a canopy | simpler |

### 4d in three steps

| Step | What | Cost grows with |
|---|---|---|
| 4d-i | SMRT through the page table: rays (`virtualShadows.rays` 8, 0 = one hard comparison), samples (`samplesPerRay` 8), the clamp, the bias, extrapolation (`extrapolateSlope` 5, 0 off); marking down to 12 texels | pixels on pages x rays x samples |
| 4d-ii | Wide penumbrae and missing pages: a sample past 12 texels of its point's level, or on no drawn page, reads a coarser level (`rayLevels` 4 marked under each pixel); then the cascades; the pool's load coarsens levels; F4 counts them | pixels on missing pages; a table read a level tried |
| 4d-iii | The adaptive ray count (a few rays first, the rest where they disagree), measured for its gain and checked for crawling on moving shadows | pixels in penumbrae |

- **Mirrored maths** (`virtual_shadows.glsl`, `render/gpu/virtual_shadows`): a ray's disc point, a sample's share, its texel and depth in the level, the clamp, the footprint's page for a sample, the blocked test with extrapolation, a point in a coarser level, the load's bias. Reads stay in `scene_surface.glsl`.
- **Tests:** a straight edge h metres up: the visible share across it against the disc's exact segment area (many rays, every level); contact hardening (a blocker at the receiver hard, higher softer, width h tan α until the cap); two overlapping blockers leak without extrapolation and not with it; samples within 12 texels, on the four pages; a coarser level's texel and depth the same world point; settings and F4.
- **Expected:** the penumbra as wide as the cascades' where both are unclamped, right under layered leaves; noise left to TAA. Cost: about 1 ms at 1440p with 8 x 8 where casters fill the view (Cinnabar's 0.57 ms at 1080p on a faster GPU), less with 4d-iii; measured at the four spots.
- **The command line:** `--set engine.render.virtualShadows.rays=N`, `samplesPerRay`, `extrapolateSlope`, `poolLoad`, all in `VirtualShadowSettings::visit` (F5, `settings.cfg`).

### Wide penumbrae: rays into coarser levels (researched Nov 25 2026, after 4d-i)

4d-i clamps a ray to 12 texels of its point's level. Near the camera that is about 5 cm, so a caster 100 m up (a rock pillar's edge, a real penumbra of 47 cm) casts about ten times too sharp; the cascades' PCSS was capped the same (3-8 cm near the camera). The user saw it on the first look.

- **Unreal (UE 5.9 docs):** "Fewer shadow map samples ... limit the amount of penumbra softness that can be achieved", so the softness is bounded by the samples along a ray, not by a texel count; the penumbra is clamped "to avoid sample divergence from the ray origin that can become increasingly 'bent'", and too large a Source Angle can "cause shadow penumbrae to visually warp as the camera gets near to them" (`SMRT.MaxRayAngleFromLight`, `RayLengthScaleDirectional`; no defaults given). Directional lights "mark a series of pages at various low detail levels to form some coarse clipmaps" (`MarkCoarsePagesDirectional`, `FirstCoarseLevel`, `LastCoarseLevel`), and "coarse pages can create performance issues" ([VSM docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine)).
- **Inferred, not confirmed** (the source is not public, the two write-ups quoting it were deleted): bending and warping near the camera are what samples read from coarser clipmap levels look like, each level snapped and scaled differently; the docs' coarse pages give those samples something to read. [I]
- **Cinnabar:** one level a point, rays 20 m long, the marking widened by the light's size times the ray's length ([PR #421](https://github.com/aszecsei/cinnabar/pull/421)): one level carries the whole penumbra, so its marked square grows with it.

#### What it means here

- **A sample's level by the ray's spread:** level L + k, the first where the sample's sideways offset is within 12 of that level's texels (by doubling, exact); a ray's end at most `rayLevels` (K) levels coarser, so the widest penumbra is 12 x 2^K texels of the point's level (K 4: 192 texels, about 0.8 m a few metres from the camera, the pillar's 47 cm whole).
- **Marking:** each pixel also asks for the pages round its point (12 texels) in its K coarser levels. A level k coarser has pages 2^k times wider, so the pixels of one level mark about 1/4 + 1/16 + 1/64 + 1/256 (a third) as many pages again; F4 measures it against today's (898 at the screenshot's spot, a pool of 4096).
- **The march in metres:** each level has its own depth scale and offset, so the march keeps heights along the light in metres from the point (stored depth less the point's depth in that level, over its scale); the tolerance is that level's bias. Slopes are metres a metre as before.
- **The same lookup as missing pages (4d-ii):** a sample in a level not drawn there reads the next coarser drawn level; past all, skipped.
- **Seams:** where samples change level the penumbra steps a little (Unreal's "bent"); the noise and the temporal pass hide most of it, a test bounds it against the disc.

#### Picks

| | Rays into coarser levels (picked) | A wider cap in one level | Coarse clipmaps marked whole (Unreal's coarse pages) |
|---|---|---|---|
| Cost grows with | pages: about a third more at K 4; rays x samples unchanged | pages: the reach squared (48 texels: each pixel near more page edges), 4x wider at most (the margin caps it below 64) | pages: whole coarse levels round the camera, hundreds a level |
| Current engines | Unreal's coarse levels and bent samples [I] | Cinnabar (20 m rays in one level) | Unreal (for fog and translucency [S]) |
| Reason | full widths near the camera for a third more pages | cannot reach the pillar's width | pays for every column, not those pixels sample |

## Decided (the user, Nov 24 2026)

1. Writing depth: clip distances with the hardware test tried first; fragment atomics as the fallback, both behind a switch in 4c and measured at the four spots. Not found elsewhere: Unreal writes atomics even for meshes without Nanite (its software rasteriser came first); EEVEE's per-tile views copied whole meshes, where a cluster mostly lands in one page; few shadow fragments stack on most scenes, our crowns about 100.
2. GPUs without mesh shaders keep today's cascades.
3. Fog, probes and cards read the far map, which also stands behind missing pages (after any coarser level); Unreal-style coarse pages would mark the fog's volume and the probe's every direction (a level is 1024 pages) for blurry answers. Revisit if the two systems or the jump to 2.7 m texels show.
