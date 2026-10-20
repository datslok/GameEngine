# Lumen: research notes

Notes on how Unreal Engine 5's Lumen global illumination is built and tuned, and what a Lumen-like layer would mean for this engine. Like [ILLUMINATION.md](ILLUMINATION.md) (DDGI), this records what the sources say, with their numbers, and keeps our own inferences apart, under "What this means for the engine". The plan itself stays in [ROADMAP.md](../ROADMAP.md).

Read in October 2026, from the sources below. Numbers tagged with a source come from it; **(unverified)** marks what could not be checked against a primary source; **(derived)** marks our own arithmetic on published numbers.

## Sources

- **[L22] Wright, Narkowicz and Kelly, "Lumen: Real-time Global Illumination in Unreal Engine 5"**, SIGGRAPH 2022 Advances in Real-Time Rendering. https://advances.realtimerendering.com/s2022/ (`.pptx` with speaker notes, 199 slides, and a `.pdf`). The main source. Read in full (text and notes). Slide numbers below are the deck's order.
- **[R21] Wright, "Radiance Caching for Real-Time Global Illumination"**, SIGGRAPH 2021 Advances. https://advances.realtimerendering.com/s2021/ (`.pptx` with notes, 118 slides). The screen probe gather and world radiance cache in detail. Read in full.
- **[JTL] Narkowicz, "Journey to Lumen"**, blog, August 2022. https://knarkowicz.wordpress.com/2022/08/18/journey-to-lumen/. The prototypes and what was dropped. Read through a summary.
- **[FN] Wright and Narkowicz, "Lumen brings real-time global illumination to Fortnite Battle Royale Chapter 4"**, Epic tech blog, January 2023. Read in full (web archive).
- **Epic's documentation** (read through summaries; the pages now say UE 5.8): **[TD]** Lumen Technical Details, **[PG]** Lumen Performance Guide, **[GIDOC]** Lumen Global Illumination and Reflections, all under https://dev.epicgames.com/documentation/en-us/unreal-engine/. **[RN5.5]-[RN5.7]** the release notes' Lumen sections.
- **[INI]** A copy of `BaseScalability.ini` (version not stated, probably 5.1-5.4): https://gist.github.com/AltimorTASDK/2c59a9f2a69ede0c3ea2c17da0d4830b. Defaults of single variables from a wiki generated from 5.4.4's source: https://indxzero.github.io/ue544cvarwiki/.
- Alternatives, read for comparison: **[GI1]** Boissé et al. 2023, "GI-1.0: A Fast and Scalable Two-level Radiance Caching Scheme for Real-time Global Illumination" (arXiv 2310.19855; code in AMD's Capsaicin); **[BRX]** AMD FidelityFX Brixelizer and Brixelizer GI (GPUOpen manuals, MIT); **[KAJ]** kajiya's GI overview (github.com/EmbarkStudios/kajiya, `docs/gi-overview.md`); **[RSG]** Ouyang et al. 2021, "ReSTIR GI" (HPG); **[SRC]** Freeman, Sannikov and Pitt-Francis 2026, "Split Radiance Cascades" (arXiv 2607.20384); **[SHARC]** NVIDIA's SHARC hash-grid cache (github.com/NVIDIA-RTX/SHARC); **[NRD]** NVIDIA's denoisers (github.com/NVIDIA-RTX/NRD); **[SVGF]** Schied et al. 2017.
- Not read: Netzel and Costa, "Ray Tracing Open Worlds in UE5" (SIGGRAPH 2022 Advances), cited by [L22] for the console ray tracing details.

## The idea in one paragraph

Lumen's budget is about half a ray per pixel, while a clean picture needs about 100 rays per pixel outdoors and 500 or more indoors [R21 s11-14]. It closes that gap in three ways. It solves the **first bounce** (what the camera's pixels receive) with dedicated, expensive techniques, and **every later bounce** cheaply through a cache that gathers from itself, one more bounce each frame [L22 s11]. It splits the light a pixel receives **by distance**, since near light changes quickly and must be sharp while far light changes slowly and may lag: contact shadows to 16 pixels, screen probes to about 2 m, world probes to 200 m, then the sky [L22 s148, s168-169]. And it traces from **probes placed on the visible pixels themselves**, so near light is what that surface sees, then filters in the probes' own space, where a tiny filter acts like a huge one on screen. Each ray runs through a chain of tracers, cheapest first, each handing the ray on where it stopped [L22 s22].

## Why not irradiance probes (DDGI)

Epic tried an irradiance field like ours and dropped it as the main gather [R21 s16-17; L22 s143]: "Irradiance was computed at the probe, not the pixel, which causes leaking and over-occlusion and makes the probe placement very difficult"; slow updates, low spatial resolution and "a distinctive flat look"; "irradiance near occluders is higher frequency than spatial probe resolution". They also dropped per-pixel tracing with a screen denoiser (SVGF, ReBLUR): the input at 2 rays per pixel was visibly noisier than the screen probes' at half a ray, since "the denoiser is running after the sampling has been done" [R21 s28], and prefiltered cone tracing against distance fields (leaks and over-occlusion "unavoidable", it "can never resolve lighting through small distant window") [L22 s139-145]. Voxel cone tracing was "leaky" [L22 s39].

Yet in UE 5.8 Epic brought an irradiance gather back for the low end: the **Irradiance Final Gather** (`r.Lumen.FinalGatherMethod 0`) "places World Space Radiance Cache probes around pixels, pre-calculates their irradiance", faster and lower quality, for the Medium setting, Switch 2 and low-end PCs at 60 fps [PG]. So Lumen today is screen probes over world probes at the top, and something close to DDGI at the bottom.

## The tracers and the hand-off

Each tracing method "hands off to the next, by writing out how far the ray traversed [TraceDistance], and whether it found a hit [bHit]. The next tracing method can then resume the ray" [L22 s22].

- Software chain: screen trace, then mesh distance fields (the first 2 m), then the global distance field, then the sky [L22 s46].
- Hardware chain: screen trace, near-field ray trace (shaded from the surface cache), far-field retrace of the misses, optionally a sorted retrace with full material shading ("hit lighting"), then the sky [L22 s129].

**Screen traces first** [L22 s23-28]. They hide mismatches between what is drawn and what is traced, and they catch what the tracers lack (software tracing has no skinned meshes, yet characters still cast indirect shadows). The trace walks the closest-depth mip chain (a hierarchical Z buffer), since linear steps skip thin occluders. Diffuse rays trace at half resolution. On hand-off the ray steps back to its last unoccluded point, or the next tracer leaks. After screen tracing the surviving rays are compacted in Z-order inside 128x128 groups, which kept them coherent and made reflection tracing 21% faster than compaction that scrambles them [L22 s29-30].

**Software ray tracing** [L22 s32-53]. A mesh signed distance field per mesh, built at import (nearest triangle by Embree, sign from 64 rays per voxel counting back faces; 0.6 ms for a 1.5 M triangle mesh), stored as sparse 8^3 bricks holding a narrow band of plus or minus 4 voxels in one byte, mipmapped and streamed into a fixed 320 MB pool. Long incoherent rays through a software BVH or grid were "too slow", so meshes are traced only for the first 2 m and the rest goes to a **global distance field**: 4 clipmaps of 256^3 round the camera, sparse bricks, far clipmaps updated less often, static and moving objects cached apart "so when a car moves we don't need to recompute static buildings", plus a quarter-resolution dense mip rebuilt each frame with 5 iterations of eikonal propagation to skip empty space. A global distance field hit does not know which mesh it hit, so lighting there comes from 4 clipmaps of 64^3 voxels holding radiance in 6 directions. Robustness: open meshes get a virtual surface 4 voxels behind; walls thinner than a voxel let rays through ("even a single ray going through a wall can cause massive light leaks"), so the field is expanded by half a voxel diagonal, growing linearly from 0 at the ray's start to keep contact shadows. Content rule: walls at least 10 cm thick [TD]. Since 5.6 mesh distance field tracing is deprecated and Epic recommends hardware ray tracing to scale past the global field [RN5.6]; [PG] still calls the global field "the fastest tracing method in Lumen".

**Hardware ray tracing** [L22 s88-136]. The fast path forces everything opaque (no any-hit shader), keeps a 20-byte payload (hit distance, material id and flags, normal, primitive, instance) and looks up the hit's light in the surface cache, which gives direct and indirect light in one fetch and fixed the missing sky occlusion of UE4's reflections. "Partial dynamic evaluation was just as expensive as full", so only two modes ship: surface cache, and hit lighting (materials and direct light evaluated at the hit, indirect still from the cache, shaded after sorting the hits). In The Matrix Awakens on PS5 at 1080p reflection tracing took 2.44 ms with the surface cache against 11.54 ms with hit lighting, "and Hit-Lighting did not provide a substantial qualitative benefit". Inline ray queries are used where possible (consoles expose the geometric normal; "significant optimization on Xbox Series X|S and PS5"). The top-level structure is rebuilt each frame with "100K instance limit, but 1M expected instances"; [PG] still advises under 100,000 instances after culling. Overlap is the weak spot: in "Lumen in the Land of Nanite", about 100 overlapping meshes at every point, the final gather took 10.03 ms with hardware tracing against 0.94 ms with the global distance field: "we could have never shipped with it". The **far field**: merged distant meshes (HLOD) under their own ray mask, traced only for the near field's misses, from 200 m out to 1 km [TD]; in the Matrix it cost 0.60 ms beside the near field's 1.77 [L22]. Epic now recommends hardware tracing "as a default option on consoles for both 30 fps and 60 fps" [PG], and 5.6 made it match the software mode's budget [RN5.6].

## The surface cache

Cards began as a tracing structure (heightfields from orthographic captures) and were dropped for tracing because gaps in their coverage leak; kept for lighting, "areas that are missing coverage only result in lost energy, instead of leaking" [L22 s5-7]. Why a cache at all: distance field hits have no UVs or attributes, and the cache stores the material, direct light and further bounces at a cost Epic controls directly [L22 s8, s57].

- **Cards** [L22 s59-63; TD]: axis-aligned rectangles of surfels found at import (0.2 ms for a 1.5 M triangle mesh), at most 12 per mesh by default; skinned meshes get 6 round their bounds. Small overlapping instances are captured together.
- **Atlas** [L22 s64-69]: a 4096^2 physical atlas of 128^2 pages; low-resolution pages always resident for GI, high-resolution ones on demand from the GPU's record of which pages rays hit, for reflections. Captured by rasterising each mesh orthographically, 512^2 texels a frame by priority (distance, last use), 10 times faster with Nanite. Layers: albedo, opacity, depth, normal, emissive, then lighting; about 90-100 MB at 4096^2.
- **Lighting it** [L22 s72-79]: a fixed budget a frame, chosen by priority = last used minus last updated through a GPU radix-sort histogram: 1024^2 texels of direct light (8x8 tiles, up to 8 lights a tile, a 1-bit shadow mask from shadow maps, the rest by ray) and 512^2 of indirect. The indirect ("radiosity") is a final gather in surface space: one 4x4 hemisphere probe per 4x4 texels, jittered over 4 frames, reading this frame's direct light and last frame's indirect, so bounces add up by feedback; under a sixteenth of Lumen's budget. Rays from a texel that hit back faces return black, so texels buried in geometry go dark and bilinear filtering does not leak.

## The screen probe final gather

The heart of Lumen, measured on PS5 at 1080p internal (TSR to 4K) at half a ray per pixel [R21 s102]:

| Pass | ms |
|---|---|
| Place probes | 0.13 |
| Generate rays (importance sampling) | 0.35 |
| Trace | 1.07 |
| Filter probes | 0.24 |
| World radiance cache | 0.53 |
| Interpolate and integrate | 0.62 |
| Temporal filter | 0.32 |
| Bent normal (short-range AO) | 0.39 |
| **Total** | **3.74** |

"About half tracing, half placement, interpolation and integration." At an eighth of a ray per pixel (probes at 1/32 resolution, the world cache at 1/128, no bent normal) it took 2.15 ms [R21 s103]; at 2 rays per pixel on an RTX 2080 Ti, 4.23 ms [R21 s110].

- **Probes** [R21 s33-37]: octahedral, typically 8x8 (64 rays), in world-space directions so neighbours' texels line up; radiance and hit distance in an atlas with borders. Placed on the depth buffer's pixels themselves ("no leaking"): one every 16 pixels, then more at 8 and 4 pixels wherever interpolating from the existing ones would fail (depth or normal edges; up to half again as many, `AdaptiveProbeAllocationFraction` 0.5), the last level by flood fill. The grid and the directions are jittered every frame, and the temporal filter and TAA average the jitter away.
- **Importance sampling** [R21 s43-55; L22 s157-159]: the lighting estimate is last frame's probes reprojected (the world cache at disocclusions), the BRDF estimate is gathered from every pixel that will read the probe. Rays start uniform; sorted by the product, "for every 3 rays with PDF below cull threshold, supersample the matching high PDF ray" at the next octahedral mip, so the ray count stays fixed. Only the BRDF may cull (the lighting estimate is noisy). "The quality of tracing four times more rays without actually tracing any more rays." One workgroup per probe.
- **Spatial filter in probe space** [R21 s57-64]: "3x3 in probe space vs 48x48 screen space". Each probe averages the same octahedral texel of its neighbours, weighted by depth only (incoming light does not depend on the receiver's normal), rejecting a neighbour's ray when its reprojected hit point lies at too wide an angle from our direction. That test favours distant light (no parallax), so "clamp neighbor hit distance to our own before reprojection", which restores contact shadows.
- **To pixels** [R21 s38-39, s89-92]: each probe becomes third-order spherical harmonics (9 coefficients) for diffuse; each pixel blends nearby probes weighted by their distance from its plane, the offset jittered too. Rough specular samples GGX directions from the probes' radiance.
- **Short-range AO** [R21 s93-97; L22 s154, s167]: a full-resolution bent normal from quick screen traces to about 16 pixels (the probe spacing), combined as horizon-based indirect lighting (Mayaux 2018), applied after the temporal filter so contact shadows never lag. Since 5.6 at half resolution with its own denoiser.
- **Temporal filter** [R21 s98-100; L22 s166-167]: history rejected by depth and normal only, no neighbourhood clamp (which would fight the jitter), so stable but ghosting; fixed by recording how fast the hits were moving: pixels whose probes mostly hit moving objects switch to a weak temporal and strong spatial filter. Up to 10 frames accumulated (`Temporal.MaxFramesAccumulated`) [INI wiki]. A firefly clamp on ray brightness (`MaxRayIntensity`, lowered from 40 to 10 in 5.6-5.7).

## The world space radiance cache

[R21 s66-85; L22 s161-164]. Distant light gets "separate sampling": noise from a small bright source grows with distance, long incoherent rays are slow, and distant light changes slowly and is shared by nearby screen probes. 3D clipmap grids round the camera (4 clipmaps, the first 25 m across [INI wiki]), sparse (only cells some screen probe will read), each probe typically **32x32 radiance texels**, 16 times the screen probes' directions. The screen probe's ray is shortened (about 2 m) and on a miss reads the world probes; the world probe's ray starts past the interpolation footprint, so a probe never sees the points that read it; parallax is corrected by reprojecting onto the probe's sphere. A fixed number of probes is traced a frame by GPU priority queue, "fixed update cost, variable lighting latency" (300 at High and Epic, 1000 Cinematic [INI]; too few gives pops when the camera moves fast [PG]). It "has stable error because the probe positions are stable, so it's easy to hide". Its directional resolution is what lit The Matrix Awakens' night mode, lit by emissive meshes alone [L22 s190]. It also guides the screen probes' importance sampling and lights hair, translucency and glossy reflections.

## Reflections

[L22 s175-187]. Rays importance-sample the visible GGX lobe, trace through the same chain, then spatial reuse (neighbours reweighted by their BRDF), temporal accumulation, a bilateral filter only where variance stays high (double strength where history was lost), then TAA. By roughness: 0.4 to 1 traces no rays and samples the screen probes (reflection cost down 50-70%); 0.3 to 0.4 traces a shortened ray and falls back to the world cache; below 0.3 full rays [L22 s182-184; PG]. Foliage has its own threshold, and 0 there is "a free win" [PG]. Tiles classify the sky and the reused areas away.

## Translucency and fog

[L22 s170-173]. A froxel grid over the view traces 3x3 or 4x4 octahedral probes per visible froxel, filters, accumulates and stores second-order spherical harmonics, which forward translucency and volumetric fog interpolate; about an eighth of the opaque gather's budget, with its own 16x16 world cache, nearly free since it overlaps the opaque one's dispatches. Mirror reflections on the front translucent layer rerun the reflection pipeline on a peeled layer.

## Budgets, quality levels and measured cost

Targets: 30 and 60 fps on consoles with **8 ms and 4 ms** for GI, reflections, translucency and fog at 1080p internal; **each lower level costs about half the one above** [PG]. The levels [PG; L22 s192; INI]:

| Level | Target | What it does |
|---|---|---|
| Cinematic | offline | probes every 8 px with 16x16 rays, 1000 world probes a frame |
| Epic | 30 fps, about 8 ms | probes every 16 px (a quarter ray per pixel), 1 ray per pixel reflections, mesh distance fields on, 32x32 world probes |
| High | 60 fps, about 4 ms | probes every 32 px (a sixteenth of a ray per pixel), reflections at a quarter ray per pixel (one per 2x2), global field only, 16x16 world probes |
| Medium | 60 fps, Switch 2 and medium PCs | the Irradiance Final Gather (5.8; older versions turned Lumen off here), Lumen reflections off (screen-space reflections) |
| Low | | Lumen off: unshadowed sky light dimmed by 0.7 to roughly match |

The rays per probe stay 8x8 = 64 at High and Epic; the levels change counts (probe spacing, world probe resolution and budget, texel budgets, mesh tracing on or off), not the algorithm, and the filters stay the same. Epic chose 1080p and upscaling over native 4K at lower settings, for "much better final image quality" [L22].

Measured totals, 1080p internal [L22 s193-196]:

| Content | Level | Surface cache | Final gather | Reflections | Total |
|---|---|---|---|---|---|
| Lumen in the Land of Nanite, PS5, global field | High | 0.47 | 2.07 | 0.23 | **2.77 ms** |
| | Epic | 0.93 | 3.46 | 0.24 | **4.63 ms** |
| Lyra, PS5, software | High | 0.36 | 1.65 | 2.28 | **4.29 ms** |
| | Epic | 0.66 | 3.30 | 4.74 | **8.70 ms** |
| The Matrix Awakens, PS5, hardware and far field | High | 1.03 | 3.05 | 2.30 | **6.38 ms** |
| | Epic | 1.17 | 5.52 | 4.62 | **11.31 ms** |
| Lake House, RTX 2080 Ti, hardware | Epic | 1.68 | 4.01 | 1.56 | **7.25 ms** |
| | 1 ray per pixel | 1.68 | 8.01 | 1.56 | **11.25 ms** |

Fortnite Chapter 4 shipped Lumen at 60 fps on PS5 and Series X|S in 4 ms (software tracing), its biggest single win all of Lumen on async compute (-0.8 ms, "ray tracing dispatches tend to have long tails") [FN]. The first prototype took 25.56 ms on a Radeon RX Vega 64 at 1080p [JTL].

## Tuning and content rules

- **Settings players and artists see** [GIDOC; TD]: Final Gather Quality (probe density; about 4 for noisy interiors is common advice), Lumen Scene Lighting Quality and Detail, Scene View Distance (200 m, up to 800 m in software), Max Trace Distance (200 m; too short leaks light into caves and large rooms), Scene Lighting and Final Gather Update Speed (raise them for a big change such as the sun switching, then lower again: global changes can take "multiple seconds" otherwise), Diffuse Color Boost and Skylight Leaking (art controls, not physical; Fortnite asked for both [FN]), reflections' Max Roughness To Trace (0.4).
- **Content** [TD; GIDOC]: walls at least 10 cm; walls, floors and ceilings as separate meshes ("importing an entire room as a single mesh is not expected to work"); small, very bright emissive surfaces are noisy, so use real lights for them, or mark them as light sources; interiors lit by a small distant window are the hard case, which the world cache's many directions solve [L22]; under 100,000 ray tracing instances, overlapping meshes excluded.
- **Artefacts and fixes** [PG; TD; L22]: noise indoors (more rays, Epic over High: "more accurate indirect shadows, and better temporal stability"); ghosting (the hit-speed fast mode, AO after the temporal filter); pops on fast camera moves (a larger world probe budget); leaks (thicker walls, the half-voxel expansion, back faces black); sky occlusion lost past 200 m (the far field).

## The alternatives

| Technique | What it is | Published cost | Fit here |
|---|---|---|---|
| **GI-1.0** [GI1] | Lumen's design made public: one screen probe per 8x8 tiles (refreshed over 4 frames), 8x8 octahedra, reprojection with plane and normal tests (`|dot(x_probe - x_p, n_p)| < cell_size`, `dot(n_probe, n_p) > 0.95`), rays moved from reused tiles to empty ones, a hash-grid world cache | 1.9-3.4 ms at 1080p, a quarter sample per pixel, RX 6900 XT / RTX 3080; tracing the smallest part, caching and resolve the most; interpolation and denoise 0.55-0.76 ms | The best public blueprint; our cascades can stand in for its hash grid |
| **kajiya** [KAJ] | Per-pixel ReSTIR GI at half resolution (one ray per 2x2 pixels) over a world-space irradiance cache (12 clipmaps of 32^3) | Indirect diffuse 2.3 ms plus cache 0.55 at 1080p, RX 6800 XT | Close in spirit; needs a denoiser |
| **ReSTIR GI** [RSG] | Each pixel keeps a reservoir (sample, weight sum, count) and resamples its neighbours' and last frame's hit points, correcting with the reconnection Jacobian | 9-166 times lower error than plain path tracing at one sample; about 0.9 ms over it at 1080p, RTX 3090 | Later, for small bright sources and glossy bounces |
| **Brixelizer GI** [BRX] | Simplified GI-1.0 tracing sparse distance fields in compute (64^3 cascades of 8^3 bricks, built from triangles at run time with an eikonal fill), hits lit from last frame's picture | Not published | The model for our software tracer (GPUs without ray tracing) |
| **Split Radiance Cascades** [SRC] | Probes whose spacing doubles and directions quadruple per cascade (near light: sharp in space, coarse in angle; far light the other way), rays split between cascades by distance | 8.6-11.5 ms, RTX 3080 Laptop | Research; watch, do not build |
| **SVGF, NRD** [SVGF; NRD] | Per-pixel denoisers | NRD 2.55-3.25 ms at 1440p, RTX 4080 | Not needed with screen probes, which denoise before integrating |
| **SHARC** [SHARC] | A world-space hash grid of radiance at path vertices, jittered | 4% of pixels update it; 160 MB | Our cascades already do this job |

## What this means for the engine

Our inferences, not the sources'.

- **Our DDGI cascades are already Lumen's two outer layers' cousin**: a world-space cache round the camera, stable, multi-bounce through feedback (Lumen's surface cache radiosity does the same through last frame's light), with a fixed budget and priorities (our probe states, ray budget and scene events). What we lack is the **inner layer**: rays traced from the visible pixels, which is what removes probe parallax, leaks at contact, and the "flat look", and gives indirect shadows smaller than the 0.5 m probe spacing. That is the roadmap's 6e "short-range lighting per pixel" and phase 14's "Lumen-like global illumination", and these notes say they are one feature.
- **We need no surface cache.** It exists to light hits that software tracing cannot shade. With hardware ray queries our probe rays already shade their hits from the GPU scene: the material's albedo (bindless textures) times the probes' irradiance at the hit, plus emissive, the sun by a shadow ray and the brightest lamps (`ddgi_trace.comp`). That is Lumen's "surface cache lookup" in a cheaper form, the probes being our cache of indirect light at hits. Epic measured full hit lighting at 4.7 times the cost for little gain; we are already between their two modes.
- **We need no mesh distance fields** (Epic has deprecated tracing them). For GPUs without ray tracing the model is a **global distance field** (clipmaps of sparse bricks, as Lumen and Brixelizer), not the voxel grid our roadmap names: Epic tried voxels (cone tracing, then bit bricks) and dropped them as leaky and slow.
- **Store radiance, not only irradiance, if screen rays are shortened.** Lumen's screen rays stop at about 2 m and read the world cache's radiance in the ray's direction. Our cascades hold irradiance (light arriving at a surface facing a direction), which is not the light along a ray. Two ways: trace the screen probes' rays to full length and shade the hit with the cascades' irradiance (simplest; hardware rays on an RTX 5080 are cheap, and our probe pass already traces 0.5 to 0.8 million rays a frame), or add a radiance atlas to the cascades later. Start with full-length rays.
- **The prerequisites are pieces we planned anyway**: per-pixel normals (the depth prepass gains a small normal target; probes need a surface to stand on, and the forward pass has no G-buffer), a hierarchical depth buffer for screen traces, and above all **TAA** (6e's first item), which Lumen relies on to average away the jittered probe placement and directions. Motion vectors also give the reprojection the temporal filter and importance sampling need.
- **The main pass would read a per-pixel GI image** instead of sampling the probes itself: the prepass gives depth and normals, compute passes place, trace, filter and integrate the screen probes, the main pass adds the result. The probe sampling in `triangle.frag` stays for the far field and for frames without the screen layer.

### A plan in stages

Each stage stands on its own and leaves the picture working; each a setting, the golden images unchanged with it off.

1. **Prerequisites** (6e): motion vectors and TAA; a normal target in the depth prepass; a hierarchical depth buffer.
2. **Screen probes, diffuse** (the core; Lumen's numbers): a probe every 16 pixels on the depth buffer, plus adaptive ones at depth and normal edges; 8x8 octahedral, world-space directions; full-length ray queries whose hits are shaded exactly as probe rays' are (one shared function), misses the sky; a 3x3 filter in probe space with depth weights and the neighbour's hit distance clamped to ours; spherical harmonics per probe; pixels blend probes by plane distance; a temporal filter rejecting by depth and normal. Mirrored on the CPU and tested like every shader library (octahedral maps and SH we have or can share).
3. **Importance sampling** (four times the rays for free): last frame's probes reprojected, the cascades at disocclusions, the BRDF gathered from the pixels; three culled rays refine one important one.
4. **Short-range AO**: a bent normal from screen traces to 16 pixels, applied after the temporal filter.
5. **Reflections from the same machinery** (6e's reflections item): rough surfaces (above 0.4) from the screen probes, smooth ones by their own GGX rays shaded the same way, with spatial reuse and a temporal filter.
6. **Later**: shortened rays with a radiance atlas in the cascades; the fast mode for pixels whose rays hit moving things; ReSTIR for small bright sources; the software tracer as a global distance field.

### Presets

Lumen's ladder, in our terms (each level about half the cost of the next; the counts change, not the method):

| Preset | Screen probes | Rays a probe | Approximate rays per pixel |
|---|---|---|---|
| Low | off (the cascades only, as today) | | |
| Medium | every 32 px | 64 | 1/16 |
| High (default) | every 16 px | 64 | 1/4 |
| Ultra | every 8 px | 64, or 16x16 | 1 |

**Expected cost (derived, to be measured):** Lumen's whole screen gather took 3.74 ms on a PS5 at half a ray per pixel at 1080p; an RTX 5080 is several times a PS5's GPU, and our hits need no surface cache, so at a quarter ray per pixel the gather might take about 1 ms at 1080p and 2 ms at 1440p. Our hits are dearer than a cache lookup (a shadow ray for the sun, more for lamps), which pulls the other way.
