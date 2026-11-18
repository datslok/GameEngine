# Big detailed worlds at high frame rates: the GPU side

Gathered Nov 18 2026, after GPU-driven drawing made the forest's frame GPU-bound. Older GPU numbers (Nov 15, before GPU-driven drawing): MAIN ~9, AA ~3.7, CUBE ~3.1 ms on the laptop's RTX 4070 (8 GB). Not covered again here: culling, levels and batching (`GPUDRIVEN.md`), occlusion (`OCCLUSION.md`), LOD and impostors (`LOD.md`), shadow caching (`SHADOWCACHE.md`). Claims marked **[inference]** are ours, not the source's.

## What the engine already does

| Trick | Here |
|---|---|
| GPU culling, levels, indirect multi-draws, chunks | `gpu-driven.md` |
| Depth prepass, main pass shaded once a pixel (depth EQUAL) | `renderer.md` |
| Reflection probe a face a frame (`probeFacesPerFrame` 1), 256 px, coarser levels and card thinning a face | `reflection-probe.md` |
| AO at half resolution with history | `ambient-occlusion.md` |
| Shadow cache redrawn on announced changes, only the nearest cascade swaying | `sun-shadows.md` |
| Render scale + DLSS/FSR upscaling | `anti-aliasing.md`; default render scale 1.0 (DLAA) with model M |

## Frame budgets in shipped games

- DOOM: The Dark Ages (60 fps consoles, ~85% dynamic resolution): deferred texturing ~1 ms, G-buffer update 0.4, deferred lighting 0.8; variable-rate compute saved 1-2 ms, ~30% of the heaviest deferred passes ([Microsoft, 2026](https://developer.microsoft.com/en-us/games/articles/2026/04/variable-rate-compute-shaders-doom-the-dark-ages/)).
- Unreal 5 at 60 Hz: Lumen (GI, reflections, fog) 4 ms at 1080p internal; TSR cut from 3.1 to 1.5 ms on console for 60 Hz ([Lumen guide](https://dev.epicgames.com/documentation/unreal-engine/lumen-performance-guide-for-unreal-engine), [TSR](https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine)).
- AC4 volumetric fog 1.1 ms on Xbox One, run on async compute during the G-buffer ([Wronski 2014](https://www.advances.realtimerendering.com/s2014/wronski/bwronski_volumetric_fog_siggraph2014.pdf)); GTAO ~0.5 ms on PS4 at half resolution ([Jimenez 2016](https://www.iryoku.com/downloads/Practical-Realtime-Strategies-for-Accurate-Indirect-Occlusion.pdf)).
- 60 fps modes render far below output: Alan Wake 2 PS5 ~860p to 1440p; Spider-Man 2 1008-1440p dynamic, with lower shadows and cubemaps; Horizon Forbidden West checkerboard 1800p; UE5 aims at 1080p internal for 4K.
- **[inference]** A 60 fps frame: lighting/GI ~4, upscaler ~1.5, fog + AO ~1.5 ms, geometry and shading the rest. MAIN 9 + AA 3.7 is over 12 ms before shadows.

## Upscaler cost

DLSS Programming Guide 310.5, Performance mode (input a quarter of output):

| GPU | Output | J/K | L | M |
|---|---|---|---|---|
| RTX 4070 Ti | 1440p | 0.91 ms | 1.46 | 1.18 |
| RTX 4070 Ti | 4K | 1.97 | 3.18 | 2.74 |
| RTX 3070 | 1440p | 1.53 | 4.01 | 3.03 |

- FSR 3 at 1440p Quality: 0.9 ms (RX 6800 XT), 2.0 (RX 5700 XT) ([GPUOpen](https://gpuopen.com/fidelityfx-superresolution-3/)).
- **[inference]** The 4070 Laptop is ~2x slower than the 4070 Ti, and DLAA (render scale 1.0) feeds four times Performance's pixels: 3.7 ms is about what DLAA with M costs. K costs ~25-40% less than M; a render scale of 0.67 also cuts MAIN roughly with the pixel count.

## Foliage: why cut-out leaves cost

- "Masked-out pixels cost nearly as much as drawn pixels"; geometry foliage beats cards under Nanite ([Epic, Nanite content](https://dev.epicgames.com/documentation/unreal-engine/working-with-naniteenabled-content)).
- GPUs shade 2x2 quads: a one-pixel triangle runs 4 lanes, ≤25% useful; depth EQUAL removes overdraw, not this quad waste ([Fatahalian 2010](https://www.microsoft.com/en-us/research/?p=159994), [Self Shadow](https://blog.selfshadow.com/publications/overdraw-in-overdrive/)).
- Ghost of Tsushima: half the blades at distance, each twice as wide, the same cover ([GDC 2021](https://gdcvault.com/play/1027214)). Unreal's "Preserve Area" likewise.
- Unreal 5.7 Nanite foliage (Witcher 4 demo, 60 fps base PS5): trees as instanced parts (one tree 3.5 GB to 29 MB on disk), far clusters as 4x4x4 voxel bricks, wind by bones (100k bones 0.1 ms) so bounds stay tight; avoid alpha masks ([Epic](https://dev.epicgames.com/documentation/unreal-engine/nanite-foliage)).
- Horizon Forbidden West: a visibility prepass for alpha-tested foliage, shaded in compute overlapping the G-buffer and cascades ([McLaren, GDC 2022](https://www.guerrilla-games.com/read/adventures-with-deferred-texturing-in-horizon-forbidden-west)); the PC port went back to a G-buffer, "better on the GPU and more consistent".
- Call of Duty: Forward+ for normal meshes, a visibility buffer for dense simple ones, with software VRS ([Drobot 2021](https://research.activision.com/publications/2021/09/geometry-rendering-pipeline-architecture)). The closest shipped design to this engine.
- Meshlet cone culling rejects only 3-14% with LODs ([zeux](https://zeux.io/2023/01/16/meshlet-size-tradeoffs/)): little use for double-sided leaves.

## Secondary views

- GTA V's real-time cube: 6 faces of 128² every frame, ~30 draws a face, only terrain, sky, low-poly buildings ([Courrèges](https://www.adriancourreges.com/blog/2015/11/02/gta-v-graphics-study/)).
- Unity: one face a frame (14 frames a cube with mips); a culling mask dropping "rocks and plants" ([Unity](https://docs.unity.com/en-us/engine/current/manual/lighting-overview/reflections/ref-probe-performance)).
- Far Cry 4: cubes keep a mini G-buffer, relit each frame from the current sun and sky; FFXV: a static cube plus time-of-day SH. Filtering a 128² cube 0.16-0.73 ms (GTX 980, [Manson & Sloan](https://research.activision.com/publications/archives/fast-filtering-of-reflection-probes)).
- Cyberpunk 2077: 4 cascades + 4 distant levels refreshed over frames under a triangle budget; casters culled by the main view's occlusion; baked per-instance visibility at 12 times of day removed "millions" of shadow triangles ([Dybisz & Witanowski 2021](https://history.siggraph.org/wp-content/uploads/2022/06/2021-Talks-Dybisz_Shadows-Optimizations-in-Cyberpunk-2077.pdf)).
- Insomniac: each cascade's cache scrolled, only the exposed strips drawn ([Acton 2012](https://advances.realtimerendering.com/s2012/insomniac/Acton-CSM_Scrolling(Siggraph2012).pdf)); CryEngine: far cascades drawn once, recentred near their border.
- Unreal VSM: wind (WPO) invalidates pages every frame; fixes: WPO off by distance, contact shadows instead of shadow maps for grass, opaque shadow materials for leaves ([Epic](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine)).
- Only terrain facing away from the sun casts ([GPUOpen](https://gpuopen.com/learn/optimizing-terrain-shadows/)).

## Variable rate and async compute

- VRS Tier 2 (an image of shading rates): Gears 5 up to 14%, Gears Tactics 18.9% of the GPU ([DirectX blog](https://devblogs.microsoft.com/directx/gears-vrs-tier2/)); DOOM Eternal used it on its forward passes. Vulkan: `VK_KHR_fragment_shading_rate` (the 4070 has it). Rate from luminance contrast and motion ([Intel](https://intel.com/content/www/us/en/developer/articles/technical/velocity-luminance-adaptive-rasterization-vrs-tier.html)).
- Async compute: DOOM 2016 gained 3-5 ms on consoles, ~7% on AMD PC; shadow passes leave ALUs idle, the classic overlap for AO, fog, light culling ([GPUOpen 2017](https://gpuopen.com/download/GDC2017-Asynchronous-Compute-Deep-Dive.pdf)). NVIDIA overlaps less than AMD consoles.
- Shading LOD: cheaper materials far away (no normal map, specular, parallax), fog lighting reduced by distance (AC4), reflections only below a roughness (Lumen).

## What this means for the engine **[inference]**

Measure first: per-pass GPU times (F4) at the rim, the Ring and the wall, then each lever with and without.

| # | Lever | Gain (guessed) | Effort | Cost grows with |
|---|---|---|---|---|
| 1 | Render scale and DLSS model: a 0.67-0.77 scale (Quality/Balanced), K beside M compared by eye | AA 3.7 → ~1.5-2.5; MAIN by the pixels | settings only | pixels |
| 2 | Per-view budgets: the probe and far cascades without small plants and grass (a class mask and minimum size a view), opaque stand-ins or a third, coarser leaf mesh (`SHADOWCACHE.md`) for leaves in far cascades and the probe; shadows while turning measured first (0.23 ms standing after `gpu-cull-shadows`) | most of CUBE; shadow passes 30-60% | low-medium | what each view sees |
| 3 | Foliage density by distance with area kept (fewer, wider plants and cards), tighter cards trimmed to the leaf outline | 15-30% of PREPASS + MAIN | medium | pixels covered |
| 4 | Shading LOD: a cheap far material variant (no normal map, specular or parallax) | 1-3 ms in a forest | medium | pixels |
| 5 | VRS image on the main pass from contrast and motion | 10-20% of MAIN | medium | pixels |
| 6 | Async compute: AO, fog, clusters during shadows and the prepass | 5-10% of the frame, less on NVIDIA | high | — |
| 7 | Visibility buffer for cut-out foliage (Call of Duty's hybrid) | 2-4 ms where foliage fills the screen | high | pixels |
| 8 | Nanite-style clusters, software rasteriser, voxel far field | the top end | a phase | pixels |

Relit G-buffer probes (Far Cry 4) and scrolled cascade caches (Insomniac) only if 2 leaves CUBE or SHADOW high.
