# Shared mesh buffer

Research for GPU-driven drawing's stage 6 (`mesh-megabuffer`), moved before stages 4 and 5 (the user, Nov 18 2026): every mesh in one vertex and one index buffer, so a pass draws all its groups with one multi-draw per pipeline style instead of a bind and a draw each. Plan in `ROADMAP.md`; stages in `GPUDRIVEN.md`. Marked **[mem]**: from memory, not checked against a source; **[inference]**: our reasoning.

## Why: stage 3's measured cost

Laptop, release, hidden copy, GPU culling on (`gpu-cull-camera`): every pass records every group, empty ones too.

| View | Draw calls | RECORD (CPU) | Empty draws (GPU, prepass + velocity) |
|---|---|---|---|
| Rim | 2,321 → 7,497 | 1.6 → 3.5 ms | +0.3 ms |
| Ring | 2,344 → 7,510 | 1.5 → 2.9 ms | +0.4 ms |

- About 40-50 ns of GPU a draw that draws nothing; no vendor publishes a figure, so ours is the best number found.
- Stages 4 and 5 add views (probe face, mirror, shadow tiles), each another ~2,500 groups recorded: without this stage they cost about what they save.

## Sources

| Source | Layout | Vertices | Draws |
|---|---|---|---|
| Kapoulkine, niagara ([niagara.cpp](https://github.com/zeux/niagara/blob/master/src/niagara.cpp), [mesh.vert](https://github.com/zeux/niagara/blob/master/src/shaders/mesh.vert.glsl)) | One vertex and one `uint32` index buffer for the scene, uploaded once; each mesh a `vertexOffset`, each level its own `indexOffset`/`indexCount` into the same vertices | Pulled from a storage buffer by `gl_VertexIndex` | One `vkCmdDrawIndexedIndirectCount`, the count written by the GPU |
| Haar & Aaltonen, "GPU-Driven Rendering Pipelines", SIGGRAPH 2015 ([slides](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)) | Fixed 64-vertex clusters in shared buffers (AC Unity, Trials) | Fetched in the vertex shader | Any number of meshes in one multi-draw, merged by material and state hash; ~10x the instances of earlier AC games |
| Wihlidal, "Optimizing the Graphics Pipeline with Compute", GDC 2016 | Per-section buffers | De-interleaved streams for compute culling | "Draw call compaction (no zero size draws)": a string of empty draws idles the GPU ~10 µs; count = min(max, count buffer); compaction by prefix sum, then ballot |
| Doom Eternal ([frame study](https://simoncoenen.com/blog/programming/graphics/DoomEternalStudy)) | "All geometry data is allocated from a single large buffer", each mesh an offset; skinning by compute into a buffer drawn as static | | One indirect draw merging unrelated meshes |
| Unreal 5 Nanite ([forum](https://forums.unrealengine.com/t/why-is-the-nanite-vram-cache-hardcoded-to-nvidia-2gb-vs-amd-4gb/2663342)) | Cluster pages streamed into one buffer, 512 MB pool by default, kept "well below the GPU resource limit, typically 2-4GB" | Pulled **[mem]** | |
| Khronos multi-draw indirect sample ([docs](https://docs.vulkan.org/samples/latest/samples/performance/multi_draw_indirect/README.html)) | One vertex and one index buffer for all geometry; draws differ by `firstIndex`/`vertexOffset` | | |
| AMD performance guide ([gpuopen](https://gpuopen.com/performance/)) | | "Use the vertex pull method"; avoid changing vertex bindings per indirect draw | "Always compact the argument buffer and provide a count buffer if any draws are culled out" |
| Unity, Unreal skin cache ([Unity](https://docs.unity.com/en-us/engine/current/script-reference/unityengine/skinnedmeshrenderer/getpreviousvertexbuffer), [UE](https://masonstevenson-dev.github.io/posts/2024-03-12-SkinCache)) | Deforming meshes: two output buffers, current and previous, swapped each frame | | |
| VMA virtual allocator ([docs](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/virtual_allocator.html)) | Offsets within one buffer; not thread-safe; alignment passed by hand; no defragmentation **[mem]** | | |

## Vulkan facts

- `vertexOffset` (int32) counts vertices and `firstIndex` counts indices, so offsets only need their element's alignment; one pool means one vertex stride.
- `gl_VertexIndex` includes `vertexOffset` **[mem]**; `gl_BaseVertex` (`shaderDrawParameters`, Vulkan 1.1) gives the draw's `vertexOffset`, so `gl_VertexIndex - gl_BaseVertex` is the mesh's own vertex number **[mem]**.
- `vkCmdDrawIndexedIndirectCount` draws min(count buffer, maxDrawCount); `maxDrawIndirectCount` is 2^32-1 on 99% of Windows reports ([gpuinfo](https://vulkan.gpuinfo.org/displaydevicelimit.php?name=maxDrawIndirectCount&platform=windows)).
- `maxStorageBufferRange` is 4 GB on 79% of Windows reports, ~1 GB or 128 MB on the rest; keep the pool under 4 GB and check `maxBufferSize`.
- Ray tracing from the pool: the vertex address plus the mesh's offset times the stride, the index address plus its first index times 4 (4-byte alignment for both); the pool needs build-input and device-address usage.

## What this means for the engine **[inference]**

| Part | Today | With the pool |
|---|---|---|
| Storage | A vertex and index buffer per `VulkanMesh` | One vertex pool, one index pool in GPU memory, append-only (meshes are only added, between frames); grown by a bigger buffer and a GPU copy |
| Levels sharing vertices | Separate copies | A level whose vertices are its mesh's first ones uses the mesh's vertices: its own indices only |
| Vertex input | Bound per batch | The pools bound once a pass; fixed-function input kept (binding 1, the previous shape, is the same pool for still meshes) |
| Per-vertex records (wind's hanging, card thinning) | Indexed by `gl_VertexIndex` | By `gl_VertexIndex - gl_BaseVertex` |
| Cloth (deforming) | A ring of CPU-written shape buffers | Outside the pool, drawn one by one as now (few) |
| Commands | One a group, drawn one by one | In drawing order, each style's run compacted by a prefix sum to the non-empty ones, drawn by one `vkCmdDrawIndexedIndirectCount` per style and pass |
| Ray tracing | Per-mesh addresses | The pools' addresses plus offsets |
| CPU path (setting off), shadows, captures | One bind a batch | One bind a pass, commands carry the offsets |

- **Expected:** the camera's ~7,500 calls to about a dozen; RECORD back below its CPU-path 1.5 ms; the empty draws' 0.3-0.4 ms of GPU gone. Stages 4 and 5 then gain all they save.
- **Risks:** the forest's mesh bytes in one buffer (to measure; well under the 2-4 GB others stay below, expected); a growth copy stalls loading once per doubling; `shaderDrawParameters` must be on.
