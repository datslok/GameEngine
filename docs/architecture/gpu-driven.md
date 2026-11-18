# GPU-driven drawing

Covers: `src/render/gpu/cull_records`, `src/render/gpu/cull_records_shared.h`, `assets/shaders/culling.glsl`, `assets/shaders/level_of_detail.glsl`, `src/render/gpu/camera_culling`, `src/render/gpu/camera_culling_shared.h`, `assets/shaders/camera_culling.glsl`, `assets/shaders/camera_cull_flags.comp`, `assets/shaders/camera_cull_scan.comp`, `assets/shaders/camera_cull_write.comp`, `src/render/vulkan/vulkan_camera_culling`

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
| `GpuCameraCull` (160 B) | frustum, level view, sway fade end, motion and sway flags, counts, the two lists' bases |

Three passes (`VulkanCameraCulling`, timed as CULL), after the scene's buffers are written:

1. `camera_cull_flags`: a thread a candidate, `cameraCandidateFlags` (drawn: taken and this slot chosen; moving: moved, mesh deformed or swaying within reach, with the velocity pass on; thinned); a 256-wide Hillis-Steele prefix sum per workgroup; level counts.
2. `camera_cull_scan`: one workgroup sums the block totals into each block's first place, a carry between rounds; totals after the last.
3. `camera_cull_write`: each flagged candidate writes `draw | thinned bit` at its place; each group its two commands from the places at its ends.

- **Same order as the CPU:** the places are prefix sums over a fixed candidate order, so the lists equal `batchByMesh`'s entry for entry (`cullCameraOnCpu`, tested on random scenes). No atomics in the lists.
- **Draw list:** the camera's region (room for every draw) first, then the shadows', then the velocity region (as big), then captures and glass; the CPU writes zeros where the GPU writes.
- **Drawing (`PassDraws`):** prepass, main and velocity passes record every group in drawing order (style, then mesh, still before moving) with `vkCmdDrawIndexedIndirect` from the GPU's commands; empty groups still bind (until the shared mesh buffer).
- **Stays on the CPU:** glass (its cached list tested against the frustum and sorted), the mirror's choice (flat-faced draws only), shadows and captures (stages 4 and 5). HIDDEN (occlusion measurement) is not counted.
- **F4:** DRAWN, levels and triangles from the counts and commands read back a frame or two late.
- **`cull-compare`:** asks for the next frame's CPU lists too, reads the GPU's back and reports each list's matching, missing and extra entries per (mesh, style), differences near an edge apart (`compareCullList`); each call shows the last report.
- Tests: `camera_culling_test` (lists, groups, kept table, comparison, edges, setting, command).
