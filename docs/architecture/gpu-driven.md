# GPU-driven drawing

Covers: `src/render/gpu/cull_records`, `src/render/gpu/cull_records_shared.h`, `assets/shaders/culling.glsl`, `assets/shaders/level_of_detail.glsl`

The GPU choosing what each view draws (culling, levels, batching), the CPU only coarse regions and changes. Research, baseline and stages: `docs/research/GPUDRIVEN.md`; progress: `ROADMAP.md` phase 18. Stage 1 (indirect draws) lives in `gpu-scene.md`.

## Now: the records and the maths (`cull-records`)

Packed and tested, read by no pass yet: stage 3 uploads them with the culling pass.

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
- `cull_records_gpu_test` (`make gputest`): the GLSL against the mirror, cases within 1e-4 of a level threshold or 1 mm of a plane left out (the GPU's square root may round either way there).
