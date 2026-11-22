# Visibility buffer and compute rasteriser (phase 21 stage 5)

Why: with twigs on, the ring view draws 109.6M cluster triangles (cards: 18.6M); GPU 118.8 ms, PREPASS 43, MAIN 62 (laptop, Nov 22 2026). Tags: [S] source read, [I] inferred.

## Sources

- [K21] Karis, Stubbe, Wihlidal, "Nanite: A Deep Dive", SIGGRAPH 2021 Advances: https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf
- [K26] Karis, "Nanite Reyes" (2026): https://graphicrants.blogspot.com/2026/02/nanite-reyes.html
- [B] JMS55, Bevy 0.15 virtual geometry: https://jms55.github.io/posts/2024-11-14-virtual-geometry-bevy-0-15/
- [H] Hable, visibility buffer with material graphs: https://filmicworlds.com/blog/visibility-buffer-rendering-with-material-graphs/
- [X] jglrxavpok, Vulkan visibility buffer: https://jglrxavpok.github.io/2023/11/26/recreating-nanite-visibility-buffer.html
- [E] Engel, triangle visibility buffer (GDC 2016): https://www.slideshare.net/slideshow/triangle-visibility-buffer/238860894
- [N] NVIDIA `vk_lod_clusters` (compute raster `--swraster`, no numbers): https://github.com/nvpro-samples/vk_lod_clusters
- [F] UE 5.7 Nanite Foliage: https://dev.epicgames.com/documentation/unreal-engine/nanite-foliage
- [V] `VK_EXT_shader_image_atomic_int64`: https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_shader_image_atomic_int64.html ; gpuinfo ~49% of devices

## Visibility buffer

| Who | Storage | Bits | Writes |
|---|---|---|---|
| Nanite [K21] | 64-bit image, `InterlockedMax` | depth 30, visible cluster 27, triangle 7 | hardware and software paths both by atomics, no colour or depth targets (one target, the two passes overlap) |
| Bevy [B] | u64 storage buffer (wgpu lacked R64 image atomics) | depth 32, cluster 25, triangle 7 | both paths by atomics |
| jglrxavpok [X] | Vulkan `R64_UINT` image, `imageAtomicMax` from the fragment shader | depth high | depth must be the high bits, or the result follows fragment order |

- This laptop (`vulkaninfo`): RTX 4070 Laptop `shaderImageInt64Atomics`, `shaderBufferInt64Atomics`, `shaderSharedInt64Atomics` all true; the AMD iGPU image int64 atomics true. NVIDIA since driver 460. Engine today enables `shaderInt64` only.
- Overflow: Bevy's 25-bit cluster id overflowed at 33.57M clusters [B]. Ours: 109.6M triangles / ~124 ≈ 0.9M clusters a frame [I], so 25 bits hold ~37x that.
- Engel: 32-bit id buffer 7.9 MB at 1080p, 99% L2 hits [E].
- No early depth: atomics replace the depth test, so every fragment of every layer does an atomic [K21]; only cluster-level culling against the pyramid stops hidden ones.

## Compute rasteriser

- Split per cluster: Nanite software-rasterises clusters whose triangles are under 32 px long; clusters needing clipping go to hardware; "the vast majority" software in their demos [K21]. Bevy: cluster screen box under 64 px both ways, near-plane crossers to hardware (w ≈ 0 gave full-screen NaN garbage) [B].
- Layout [K21, K26]: a workgroup of 128 a cluster; each thread transforms a vertex into shared memory; then a thread a triangle: edge equations, depth gradient, screen rect, each pixel centre tested against 3 edges, `atomicMax`. Scanline form (covered x-span per row) when a triangle's x loop is over 4 px in the wave; spreading big triangles over threads was not a win: hardware wins first.
- Fill rules: Nanite matches the D3D rules (fixed point, top-left) so software and hardware clusters meet without cracks [K21]; Bevy skips both so far [B].
- Speed: 3x Nanite's fastest primitive-shader path on average, more for pure micropolygons [K21]; hardware sets up at most 4 triangles a clock, worse with primitive ids. Bevy raster 3.44 → 0.42 ms (part from better LOD); others report software only 10-20% faster than mesh shaders in most scenes [B]. **We already use mesh shaders**, so expect less than 3x [I].
- Nanite frame (~2496x1404): visibility buffer ~2.5 ms, main raster 1.15 ms, 25M triangles, 1,536,794 software clusters against 6,686 hardware [K21].
- Limits: small triangles bound by transform and setup, medium by coverage tests, large by atomics; divergence from back-face early outs and mixed sizes in a wave [K21].

## Material pass

- Nanite 2021: material ids written as a depth, then a tile grid drawn per material with `EQUAL`, ~2 ms; they meant to move to compute [K21]. UE 5.4+: compute shade binning; thousands of empty bins cost CPU [UE forums].
- Hable [H] (RTX 3070, 1080p, ~1 px triangles): forward 15.0 ms, deferred 11.2, visibility 8.85; forward shades ~4x a pixel from 2x2 quad helpers. Big triangles: visibility slightly slower (~0.33 ms fixed). Classification: count pixels a material, prefix sum, scatter, indirect dispatch a material. Incoherent vertex fetch where an 8x8 block hits many triangles.
- Derivatives: rebuilt analytically from the three clip-space vertices (chain rule), `textureGrad`; under 2% cost [K21, H].
- Wind: whatever the raster applies to a vertex the material pass applies again; motion vectors need last frame's sway too [I]. UE 5.7 moved foliage wind to bones (100k bones ~0.1 ms) for tight bounds [F].
- Nanite's output writes scene depth, motion vectors, a mask; later passes read ordinary depth [K21].

## Depth, pyramid, shadows

- Depth exported from the visibility buffer for later passes; the pyramid for the second occlusion pass built from it (BuildHZB 99 µs) [K21]. Bevy: `resolve_depth` writes the high 32 bits as depth [B].
- Virtual shadow maps use the same rasterisers, many views at once, software clusters emitted per page [K21] (our stage 4).

## Foliage

- Nanite on aggregates (leaves, grass): holes and overdraw, no per-triangle culling, "doesn't perform as well" [K21]. UE 5.7: assemblies, skinned wind, voxels for far foliage [F].
- [I] A visibility buffer removes the forward pass's quad waste (MAIN's 62 ms) but not the raster's triangle count (PREPASS's 43 ms); the compute rasteriser cuts the cost a triangle; neither cuts the count. 109.6M triangles over a ~2-3M pixel screen is ~40 a pixel, so the twigs' levels (3e) and culling of swaying twigs (3c) still decide the count.
