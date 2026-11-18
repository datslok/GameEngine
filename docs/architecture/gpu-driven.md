# GPU-driven drawing

Covers: `src/render/gpu/cull_records`, `src/render/gpu/cull_records_shared.h`, `assets/shaders/culling.glsl`, `assets/shaders/level_of_detail.glsl`, `src/render/gpu/camera_culling`, `src/render/gpu/camera_culling_shared.h`, `assets/shaders/camera_culling.glsl`, `assets/shaders/camera_cull_flags.comp`, `assets/shaders/camera_cull_scan.comp`, `assets/shaders/camera_cull_write.comp`, `src/render/vulkan/vulkan_camera_culling`, `assets/shaders/camera_cull_compact.comp`, `src/render/gpu/shadow_culling`

The GPU choosing what each view draws (culling, levels, batching), the CPU only coarse regions and changes. Research, baseline and stages: `docs/research/GPUDRIVEN.md`; progress: `ROADMAP.md` phase 18. Stage 1 (indirect draws) lives in `gpu-scene.md`.

## The records and the maths (`cull-records`)

Read by the camera's culling (below); `CULL_MOVED`, `CULL_SWAYS` and `GpuMeshLevels::changesShape` added for its velocity list.

| Record | Bytes | One per | Holds |
|---|---|---|---|
| `GpuCullRecord` | 48 | draw (the instance's index) | bounding sphere, level sphere (`levelBounds`, else bounds), `CULL_*` flags (shadow-only, hidden, transparent, casts shadows, cut-out, double-sided, thinnable), mesh number, impostor's `hiddenBelowError` |
| `GpuMeshLevels` | 48 | mesh | four slots (the mesh, then its levels coarsest last; `CULL_NO_MESH` draws nothing), each slot's error, the level count, which slots share the mesh's vertices |
| `GpuLevelView` | 32 | view | eye and pixels a metre away, pixel error, `CULL_LEVELS_*` (enabled, impostors, orthographic) |
| `GpuFrustum` | 96 | view | six inward planes |

Packed by `packCullRecord`, `packMeshLevels` (throws past three levels), `packLevelView`, `packFrustum`.

## The choices, mirrored

| C++ (`render/gpu/cull_records`) | GLSL | Equals the CPU's |
|---|---|---|
| `sphereInFrustum` | `culling.glsl` | `Frustum::intersectsSphere` (unknown bounds always in) |
| `cameraTakes` | `culling.glsl` | `visibleDraws` then `opaqueDraws` |
| `levelErrorInPixels`, `levelSphereDistance` | `level_of_detail.glsl` | `errorInPixels`, `levelDistance` |
| `chooseLevelSlot` | `level_of_detail.glsl` | `chosenMesh`: the impostor rule, then the walk to the coarsest fitting level; no slot where nothing is drawn |
| `slotThinned` | `level_of_detail.glsl` | `batchByMesh`'s thinned mark |

- `cull_records_test`: random scenes and views (perspective and orthographic, levels and impostors on and off): every draw's slot, thinning and camera choice equal the CPU functions'.
- `cull_records_gpu_test` (`make gputest`): the GLSL against the mirror (also `camera_culling.glsl`), cases near an edge (`nearAChoiceEdge`, `nearACameraEdge`: within 1e-4 of a level threshold, 1 mm of a plane or of the sway's reach) left out, since the GPU's square root may round either way there.

## The camera's lists on the GPU (`gpu-cull-camera`)

`engine.render.passes.gpuCulling`, on by default: the camera's and the velocity pass's lists built by compute in place of `visibleDraws`, `chosenMesh`, `batchByMesh` and the mover walk. Off draws the CPU's lists (the reference) until stage 5 deletes them.

| Piece | What |
|---|---|
| Candidate (`GpuCullCandidate`, 8 B) | a draw and one level slot with a mesh; every draw not glass, shadow-only and hidden included (the GPU drops them by flag, so hiding keeps the groups) |
| Group (`GpuCullGroup`, 16 B; `CullGroupStyle`) | the candidates of one (style, mesh), groups in `batchByMesh`'s order, candidates in draw order (`buildCullGroups`) |
| `CameraCullTable` | still draws' records, groups, level tables, glass and flat-faced lists kept; built ranges repack records, groups rebuilt only when a draw's mesh, style, glass, flat faces or its mesh's level meshes change; the moving draws' groups made each frame after the still ones |
| `GpuCameraCull` (208 B, one a view) | frustum, level view, sway fade end, motion and sway flags, counts, the lists' bases, its number and slices (`placeCullView`) |

Four passes (`VulkanCameraCulling`, timed as CULL), after the scene's buffers are written, each one dispatch over every view (below):

1. `camera_cull_flags`: a thread a candidate, `cameraCandidateFlags` (drawn: taken and this slot chosen; moving: moved, mesh deformed or swaying within reach, with the velocity pass on; thinned); a 256-wide Hillis-Steele prefix sum per workgroup; level counts.
2. `camera_cull_scan`: one workgroup sums the block totals into each block's first place, a carry between rounds; totals after the last.
3. `camera_cull_write`: each flagged candidate writes `draw | thinned bit` at its place; each group its two commands (its mesh's `firstIndex` and `vertexOffset` in the shared mesh buffer) from the places at its ends.
4. `camera_cull_compact`: one workgroup packs each list's commands that draw something, style run by style run of the drawing order (`cullDrawOrder`: pooled groups by style then mesh, then deforming ones), by a prefix sum, and writes each style's count (`packCullCommands` mirrors it).

**Views (`gpu-cull-captures`, `gpu-cull-shadows`):** the camera, then each probe face and the mirror drawn this frame, then each shadow tile drawn, are the dispatch's second dimension (`gl_WorkGroupID.y`), any number: passes 1-3 run in batches of `CAMERA_CULL_SCRATCH_VIEWS` (16) reusing the flags, offsets and block sums (the views bound from the batch's first), the packing over all at once.

| Piece | What |
|---|---|
| Captures' views (`CaptureCullView`) | `probeFaceCullView` (the face from its cycle's origin, `probeSize` pixels a quarter turn), `mirrorCullView` (`planarFrustum`, the mirror's picture height); the CPU's lists use the same |
| Their records (`packCaptureCull`) | no velocity list, no sway test, so a capture's velocity commands draw nothing |
| Slices (`placeCullView`, `cullViewSizes`) | each view's commands, packed commands and places after the one before's, its flags, offsets and block sums by its place in its batch; its counts and packed counts at its number; the table, groups and drawing order shared |
| Drawing | `drawProbeCapture` and `drawPlanarReflection` draw `mainDraws(frame, table, view)` with the capture pipelines (front faces culled): a call a style |
| Read back | every view's counts and both lists' commands; for `cull-compare` both lists of every view |

- **Same order as the CPU:** the places are prefix sums over a fixed candidate order, so the lists equal `batchByMesh`'s entry for entry (`cullCameraOnCpu`, tested on random scenes). No atomics in the lists.
- **Draw list:** the camera's region (room for every draw) first, then the shadows', then the velocity region (as big), then a region as big for each capture drawn (faces, then the mirror), then glass; the CPU writes zeros where the GPU writes.
- **Drawing (`PassDraws`, a view's commands from `commandFirst`):** prepass, main, velocity and capture passes bind the pools once and draw each style's packed commands with one `vkCmdDrawIndexedIndirectCount` (`drawIndirectCount`); groups of deforming meshes after, one by one from their own commands.
- **Stays on the CPU:** glass (its cached list tested against the frustum and sorted), the mirror's choice (flat-faced draws only), which shadow tiles to draw (below). HIDDEN (occlusion measurement) is not counted.
- **F4:** DRAWN, levels, triangles and the probe faces' triangles from the counts and commands read back a frame or two late.
- **`cull-compare`:** asks for the next frame's CPU lists too, reads the GPU's back and reports each list's (camera, velocity, each probe face drawn, mirror) matching, missing and extra entries per (mesh, style), differences near an edge apart (`compareCullList`); each call shows the last report.
- Tests: `camera_culling_test` (lists, capture views, slices, groups, kept table, comparison, edges, setting, command).

## Shadow tiles (`gpu-cull-shadows`)

Each shadow tile drawn is a view (`packShadowCull`): its frustum, levels by its own texels (`tileLevelView`), `CAMERA_CULL_SHADOW`, reach, smallest caster and cascade (`GpuCameraCull` 240 B). `shadowTakes` (mirrored, `camera_culling.glsl`) equals the CPU's `castersInReach`, `shadowCasters`, `castersAtLeast`, `castersForCascade` (`shadowTileCasters`); its draws' cascade limit is `GpuCullRecord::shadowCascades`. Its entries carry no thinned bit; a tile keeping its movers apart (the far map) has `CAMERA_CULL_MOTION`: casters that moved go to its second list, drawn over the patches.

**Which tiles are drawn** (`prepareGpuShadowDraws`, `shadowTileNeeds`): the CPU decides from what the scene announces, never walking every draw:

| Redrawn when | From |
|---|---|
| its view or level settings changed | its key in `ShadowTileMemory` (the tile and its `GpuLevelView`) |
| the still draws were built afresh | `CameraCullFrame::stillRebuilt` |
| a still draw built in place touches it, where it was or is | `CameraCullFrame::stillChanges` (old and new records), `shadowTakes` |
| a moving draw it takes changed | the moving draws' records (`shadowTakes`), their fingerprint in its key (standing ones only where it keeps movers apart) |
| its glass changed | `glassInReach` over the table's glass list, in its key |
| it sways (`tileSways`: the nearest cascade with sway on) | a new key every frame |

- A drawn tile's lists get room for every draw each in the draw list (one list, two keeping movers apart), after the main pass's entries; cached tiles get no view.
- F4's SHADOW DRAWN and CULLED from the counts read back (still casters counted only where redrawn).
- `cull-compare` compares every drawn tile's lists too (Shadow tile n, Shadow tile n movers).
- Tests: `shadow_culling_test` (lists equal the CPU's, the new rule redraws every tile today's fingerprint would over random changes, nothing when nothing changed, all after a full rebuild, swaying tiles every frame; edges).
