# GPU-driven drawing

Covers: `src/render/gpu/cull_records`, `src/render/gpu/cull_records_shared.h`, `assets/shaders/culling.glsl`, `assets/shaders/level_of_detail.glsl`, `src/render/gpu/camera_culling`, `src/render/gpu/camera_culling_shared.h`, `assets/shaders/camera_culling.glsl`, `assets/shaders/camera_cull_segments.comp`, `assets/shaders/camera_cull_segment_scan.comp`, `assets/shaders/camera_cull_segment_list.comp`, `assets/shaders/camera_cull_flags.comp`, `assets/shaders/camera_cull_scan.comp`, `assets/shaders/camera_cull_write.comp`, `src/render/vulkan/vulkan_camera_culling`, `assets/shaders/camera_cull_compact.comp`, `src/render/gpu/shadow_culling`

The GPU choosing what each view draws (culling, levels, batching), the CPU only coarse regions and changes. Research, baseline and stages: `docs/research/GPUDRIVEN.md`; progress: `ROADMAP.md` phase 18. Stage 1 (indirect draws) lives in `gpu-scene.md`.

## The records and the maths (`cull-records`)

Read by the camera's culling (below); `CULL_MOVED`, `CULL_SWAYS` and `GpuMeshLevels::changesShape` added for a view's moving list (shadow tiles keeping movers apart; the camera builds none since the main pass writes its own motion, `anti-aliasing.md`).

| Record | Bytes | One per | Holds |
|---|---|---|---|
| `GpuCullRecord` | 48 | draw (the instance's index) | bounding sphere, level sphere (`levelBounds`, else bounds), `CULL_*` flags (shadow-only, hidden, transparent, casts shadows, cut-out, double-sided, thinnable, fades, cluster levels), mesh number, impostor's `hiddenBelowError` |
| `GpuMeshLevels` | 48 | mesh | four slots (the mesh, then its levels coarsest last; `CULL_NO_MESH` draws nothing), each slot's error, the level count, which slots share the mesh's vertices |
| `GpuLevelView` | 32 | view | eye and pixels a metre away, pixel error, `CULL_LEVELS_*` (enabled, impostors, orthographic, clusters), fade band (0 none: every view but the camera) |
| `GpuFrustum` | 96 | view | six inward planes |

Packed by `packCullRecord`, `packMeshLevels` (throws past three levels), `packLevelView`, `packFrustum`.

## The choices, mirrored

| C++ (`render/gpu/cull_records`) | GLSL | Equals the CPU's |
|---|---|---|
| `sphereInFrustum` | `culling.glsl` | `Frustum::intersectsSphere` (unknown bounds always in) |
| `cameraTakes` | `culling.glsl` | `visibleDraws` then `opaqueDraws` |
| `levelErrorInPixels`, `levelSphereDistance` | `level_of_detail.glsl` | `errorInPixels`, `levelDistance` |
| `levelSlotWithin`, `chooseLevelSlot` | `level_of_detail.glsl` | `chosenMesh`: the impostor rule, then the walk to the coarsest level fitting a budget; no slot where nothing is drawn |
| `levelFadeSlots` | `level_of_detail.glsl` | `levelFade`: the finer and coarser slots (either none) and the share (level-of-detail.md, Level fades) |
| `slotThinned` | `level_of_detail.glsl` | `batchByMesh`'s thinned mark |

- `cull_records_test`: random scenes and views (perspective and orthographic, levels, impostors and fades on and off): every draw's slot, thinning, fade and camera choice equal the CPU functions'.
- `cull_records_gpu_test` (`make gputest`): the GLSL against the mirror (also `camera_culling.glsl`), cases near an edge (`nearAChoiceEdge`, `nearACameraEdge`: within 1e-4 of a level threshold or a fade band's start, 1e-3 of a fade field's step, 1 mm of a plane or of the sway's reach) left out, since the GPU's square root may round either way there.

## The lists on the GPU (`gpu-cull-camera`)

Every view's lists are built by compute (the only way since `cpu-culling-removed`; no setting). The CPU's `visibleDraws`, `chosenMesh`, `batchByMesh` and `shadowTileCasters` remain as the reference the tests and `cull-compare` check against.

| Piece | What |
|---|---|
| Candidate (`GpuCullCandidate`, 8 B) | a draw and one level slot with a mesh; every draw not glass, shadow-only and hidden included (the GPU drops them by flag, so hiding keeps the groups) |
| Chunk (`GpuCullChunk`, 32 B; `packCullChunk`) | `FrameDescription::stillChunkSlots` (64) still slots (chunk = slot / 64, blocks whole chunks, `scene/still_draws`): the box round its members' spheres grown by `CULL_CHUNK_MARGIN` 1 cm plus 1e-5 of its farthest coordinate, flags `CULL_CHUNK_SEEN` (a camera may take one), `CASTS` (a shadow may), `ALWAYS` (one of unknown size) |
| Segment (`GpuCullSegment`, 16 B) | a group's candidates in one chunk; moving draws' one a group, `CULL_NO_CHUNK` (never culled as a chunk) |
| Group (`GpuCullGroup`, 32 B; `CullGroupStyle`) | its run of segments (each group's in chunk order), groups in `batchByMesh`'s order: within a group, candidates in draw order (`buildCullGroups` for moving draws) |
| `CameraCullTable` | still records, chunks, level tables, glass and flat-faced lists kept; candidates kept a chunk at a time (each chunk's sorted by group then draw, in a run with a quarter spare from `SlotBlocks`); built ranges repack their records and chunks, and the candidates of chunks where a draw's mesh, style, glass or flat faces changed (`getChunkBuilds`), then the segment table and still groups (`buildSegments`, growing with the segments); a mesh's level meshes changed or a frame not following builds everything (`getStillBuilds`); the moving draws' groups made each frame after the still ones |
| Kept tables | records, chunks, candidates and segments in lasting buffers (`gpu-scene.md`): when the generation follows the one held, records, chunks and candidates by the repacked ranges (`stillRepacked`, `chunksRepacked`, `candidatesRepacked`), segments whole when they changed; all otherwise; the moving ones every frame; levels, groups, views and drawing order in each frame's upload memory |
| `GpuCameraCull` (256 B, one a view) | frustum, level view, sway fade end, motion and sway flags, counts, the lists' bases, its number, slices (`placeCullView`), segment slices and a sun cascade's thinning band |
| `GpuCullViewWork` (16 B, one a view) | written by the segment scan: visible candidates, visible segments, their workgroups |

Seven passes (`VulkanCameraCulling`, timed as CULL), after the scene's buffers are written, each one dispatch over every view (below). Chunk culling (stage 7b, research `docs/research/GPUDRIVEN.md`) runs first, so the per-candidate passes see only visible chunks' candidates:

1. `camera_cull_segments`: a thread a segment, `chunkInView` (no chunk: always); a 256-wide prefix sum of (candidates, 1) of those passing.
2. `camera_cull_segment_scan`: one workgroup a view sums the segment blocks; writes the view's work and raises its batch's indirect dispatches (`atomicMax`; the CPU starts the flags pass's at 0 and the write pass's at the groups' workgroups by `vkCmdUpdateBuffer`).
3. `camera_cull_segment_list`: each visible segment (its visible-segment count one less than the next's, so the chunk test is never made twice) writes (segment, first visible candidate) at its place: the visible list, in segment order.
4. `camera_cull_flags` (indirect, the batch's most visible candidates' workgroups): a workgroup's 256 visible candidates lie in at most 256 consecutive visible segments: one thread finds the first by binary search, the workgroup loads the next into shared memory, each thread searches there. Then `cameraCandidateFlags` (drawn: taken and this slot chosen, or either slot of a draw fading; moving: moved, mesh deformed or swaying within reach, with the velocity pass on; thinned; fading), the candidate's list place kept above the flags (`CAMERA_CULL_FLAG_BITS`); a 256-wide Hillis-Steele prefix sum per workgroup; level counts.
5. `camera_cull_scan`: one workgroup a view sums its block totals (its work's blocks) into each block's first place, a carry between rounds; totals after the last.
6. `camera_cull_write` (indirect): each flagged visible candidate writes `cameraCandidateEntry` (draw, thinned bit, fade field: `cameraCandidateFade`, read from its record only for one flagged fading) at its place; each group its two commands (its mesh's `firstIndex` and `vertexOffset` in the shared mesh buffer) from the places at its first segment's first visible candidate and past its last.
7. `camera_cull_compact`: one workgroup packs each list's commands that draw something, style run by style run of the drawing order (`cullDrawOrder`: pooled groups by style then mesh, then deforming ones), by a prefix sum, and writes each style's count (`packCullCommands` mirrors it).

**Views (`gpu-cull-captures`, `gpu-cull-shadows`):** the camera, then each probe face and the mirror drawn this frame, then each shadow tile drawn, are the dispatch's second dimension (`gl_WorkGroupID.y`), any number: passes 1-6 run in batches of `CAMERA_CULL_SCRATCH_VIEWS` (16) reusing the segment offsets, visible lists, flags, offsets and block sums (the views bound from the batch's first, each batch's dispatches 256 B apart), the packing over all at once.

| Piece | What |
|---|---|
| Captures' views (`CaptureCullView`) | `probeFaceCullView` (the face from its cycle's origin, `probeSize` pixels a quarter turn), `mirrorCullView` (`planarFrustum`, the mirror's picture height); the CPU's lists use the same |
| Their records (`packCaptureCull`) | no velocity list, no sway test, so a capture's velocity commands draw nothing |
| Slices (`placeCullView`, `cullViewSizes`) | each view's commands, packed commands and places after the one before's, its flags, offsets, block sums, segment offsets, visible list and segment block sums by its place in its batch (flags sized by the candidate list, an upper bound on any view's visible ones); its counts, packed counts and work at its number; the table, groups and drawing order shared |
| Drawing | `drawProbeCapture` and `drawPlanarReflection` draw `mainDraws(frame, table, view)` with the capture pipelines (front faces culled): a call a style |
| Read back | every view's counts and both lists' commands; for `cull-compare` both lists of every view |

- **Same order as the CPU:** the places are prefix sums over a fixed order (segments, then candidates within them), so the lists equal `batchByMesh`'s entry for entry (`cullCameraOnCpu` runs the passes' steps, chunk tests included; tested on random scenes as flat groups and as a chunked table). No atomics in the lists; the dispatch sizes' `atomicMax` decides only how many workgroups run.
- **Chunks never hide a drawn draw:** a chunk's box holds its members' spheres plus a margin past rounding, and its flags cover every view kind (`testChunksNeverHideADrawnDraw`); `cull-compare` in the forest: every list equal at the rim, Ring and wall.
- **Draw list (`DrawListLayout`):** the camera's region (room for every draw) first, then each drawn shadow tile's (and its glass), then a region for each capture drawn (faces, then the mirror), then glass. The CPU writes only its own runs (`UploadArray::writeRuns`); the GPU's regions are left for the culling, nothing reading them before.
- **Drawing (`PassDraws`, a view's commands from `commandFirst`):** prepass, main and capture passes bind the pools once and draw each style's packed commands with one `vkCmdDrawIndexedIndirectCount` (`drawIndirectCount`); groups of deforming meshes after, one by one from their own commands.
- **Stays on the CPU:** glass (its cached list tested against the frustum and sorted), the mirror's choice (flat-faced draws only), which shadow tiles to draw (below).
- **F4:** DRAWN, levels, triangles and the probe faces' triangles from the counts and commands read back a frame or two late.
- **`cull-compare`:** asks for the next frame's CPU lists too (`cpuReferenceLists`), reads the GPU's back and reports each list's (camera, each probe face drawn, mirror, shadow tiles) matching, missing and extra entries per (mesh, style), differences near an edge apart (`compareCullList`); each call shows the last report.
- **After it:** the camera's groups carry their mesh's clusters (`placedCullGroup`); `VulkanCameraCulling::outputs` and `placedView` hand its buffers and placed view to the mesh clusters' culling ([mesh-clusters.md](mesh-clusters.md)).
- Tests: `camera_culling_test` (lists, capture views, slices, groups, kept table, chunks, changed chunks alone, comparison, edges, no setting, command); `shadow_culling_test` (tiles, both forms); `gpu_scene_test` (the draw list's layout); `cull_records_gpu_test` (`chunkInView`, `boxInFrustum`, cases within 1 cm of a plane or the reach left out).

## Shadow tiles (`gpu-cull-shadows`)

Each shadow tile drawn is a view (`packShadowCull`): its frustum, levels by its own texels (`tileLevelView`), `CAMERA_CULL_SHADOW`, reach, smallest caster and cascade (`GpuCameraCull` 240 B). `shadowTakes` (mirrored, `camera_culling.glsl`) equals the CPU's `castersInReach`, `shadowCasters`, `castersAtLeast`, `castersForCascade` (`shadowTileCasters`); its draws' cascade limit is `GpuCullRecord::shadowCascades`. Its entries carry no thinned bit; a tile keeping its movers apart (the far map) has `CAMERA_CULL_MOTION`: casters that moved go to its second list, drawn over the patches.

**Which tiles are drawn** (`prepareShadowDraws`, `shadowTileNeeds`): the CPU decides from what the scene announces, never walking every draw:

| Redrawn when | From |
|---|---|
| its view or level settings changed | its key in `ShadowTileMemory` (the tile and its `GpuLevelView`) |
| the still draws were built afresh | `CameraCullFrame::stillRebuilt` |
| a still draw built in place touches it, where it was or is | `CameraCullFrame::stillChanges` (old and new records), `shadowTakes` |
| a moving draw it takes changed | the moving draws' records (`shadowTakes`), their fingerprint in its key (standing ones only where it keeps movers apart) |
| its glass changed | `glassInReach` over the table's glass list, in its key |
| it sways (`tileSways`: the nearest cascade with sway on) | a new key every frame |
| a planting cell it takes was made or dropped, or the placed plants' table changed | `PlantedShadowChanges` (`planting.md`, Placed plants drawn) |

- A drawn tile's lists get room for every draw each in the draw list (one list, two keeping movers apart); cached tiles get no view.
- F4's SHADOW DRAWN and CULLED from the counts read back (still casters counted only where redrawn).
- `cull-compare` compares every drawn tile's lists too (Shadow tile n, Shadow tile n movers).
- Tests: `shadow_culling_test` (lists equal the CPU's, the new rule redraws every tile today's fingerprint would over random changes, nothing when nothing changed, all after a full rebuild, swaying tiles every frame; edges).
