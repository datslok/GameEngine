# Cloth on the GPU

Covers: `src/render/gpu/cloth_gpu`, `src/render/gpu/cloth_gpu_shared.h`, `src/render/gpu/cloth_timing`, `src/render/vulkan/vulkan_cloth`, `src/render/vulkan/cloth_pass_stamps`, `src/engine/gpu_cloth`, `assets/shaders/cloth.glsl`, `assets/shaders/cloth_pass.glsl`, `assets/shaders/cloth_levels.glsl`, `assets/shaders/cloth_point_lanes.glsl`, `assets/shaders/cloth_shape.comp`, `assets/shaders/cloth_bend.comp`, `assets/shaders/cloth_block.comp`, `assets/shaders/cloth_coarse_step.comp`, `assets/shaders/cloth_finish.comp`, `assets/shaders/cloth_gradient.comp`, `assets/shaders/cloth_held.comp`, `assets/shaders/cloth_line_apply.comp`, `assets/shaders/cloth_line_terms.comp`, `assets/shaders/cloth_line_total.comp`, `assets/shaders/cloth_predict.comp`, `assets/shaders/cloth_spread.comp`, `assets/shaders/cloth_stitch.comp`, `assets/shaders/cloth_stretch.comp`

The CPU solver ported as it is: XPBD, or vertex block descent with its coarse levels, as each tick's settings say; tethers and stitches with either. The CPU solver is the shaders' mirror (`src/cloth/CLAUDE.md`). Self-collision is not ported. The solver itself: `cloth.md`, `cloth-solvers.md`. Recorded GPU numbers: `docs/CLOTHGPU.md`, Part 5.

- Within rounding of the CPU, not to the bit (fused multiply-adds, the GPU's own division and roots).
- Bit for bit repeatable on one GPU and driver: no pass depends on the order its invocations run.

## Records

`render/gpu/cloth_gpu`: std430, every size checked by `static_assert` against `cloth.glsl`. Field meanings: the header.

| Record | Bytes | Holds |
|---|---|---|
| point (`Vec4`, `packClothPoints`) | 16 | position, inverse mass in w |
| `GpuStretch` | 16 | an edge |
| `GpuBend` | 48 | a hinge: points, rest angle, stiffness share, isometric weights |
| `GpuCollider` | 48 | a sphere, capsule or box (`CLOTH_COLLIDER_*`), `packCollider` |
| `GpuClothPin` | 32 | where a moving pin starts and ends the tick |
| `GpuClothTether` | 8 | a tether slot; `packClothTethers` gives every point its slots, all spare without tethers |
| `GpuClothStitch` | 8 | a stitch's points, `packClothStitches`; partners per point by `clothStitchPartners` (`clothNoStitch` for none) |
| `GpuClothField` | 48 | a field body: first sample and spacing, sample counts, band (`packClothField`) |
| `GpuClothTick` | 112 | a tick's numbers, `packClothTick` |
| `GpuPointEdge` / `GpuPointHinge` | 8 / 48 | each point's edges and hinge corners, flattened so each term costs one read less (`packPointEdges`, `packPointHinges`) |
| `GpuClothLink` / `GpuClothLevel` | 8 / 32 | the coarse levels, `packClothLevels` -> `GpuClothLevels` |

- `GpuClothTick` holds everything the CPU works out once from the substep, in the same float expressions (compliances over h^2, 1 / h^2, `energyStiffness`, the drag's kept share with `exp` on the CPU), so both sides start from the same bits. Also the solver (`CLOTH_METHOD_*`), `tetherStretch` (1 + slack, 0 with tethers off), the tick's field range and `stitchClosingStep`.
- `ClothTickCommands` (`cloth/cloth.h`): what a tick is given (fabric, length, one target per pin, hand, shapes, fields); `clothTickCommands(cloth, seconds, colliders, fields)` takes it as `stepCloth` would.
- `clothPinSlots`: each point's pin, or `clothNoPin`.
- Constants: `cloth_gpu_shared.h` (group size 64, `CLOTH_POINT_LANES` 8, collider kinds, bending models, solvers, no-pin, no-grab, no-stitch markers).

## The passes

`VulkanClothPipelines`: built once, shared by every cloth. Per tick and substep, in the CPU's order, one global barrier after every dispatch (each reads what the last wrote):

| Pass | Runs | Does |
|---|---|---|
| `cloth_predict.comp` | every point | Prediction; moving pins slid. Its result is also the prediction kept for vertex block descent |
| `cloth_stretch.comp` | per stretch colour | XPBD stretch |
| `cloth_bend.comp` | per bending colour, only on substeps `bendingSubsteps` names | XPBD bending; the push constant `bendingSubsteps` divides 1 / h^2 |
| `cloth_block.comp` | per point colour | Vertex block descent: each point's terms summed over its lanes, then `clothBlockSolve` by the first lane |
| level passes | per level, below | Only with levels, between two sweeps |
| `cloth_stitch.comp` | per stitch (sewn cloths only) | Both points' hand and tethers (`clothHandAndTethers`), then the stitch. One invocation owns both points, so no invocation reads what another writes |
| `cloth_finish.comp` | every point | Hand and tethers for points in no stitch; shapes then field bodies with friction (none while the point's stitch is open, `clothStitchWidest`); velocity and drag. Writes no pinned point, since tethers read their anchors |

- **Bindings:** the six cloth passes bind the same twenty storage buffers at set 0 (`cloth_pass.glsl`), so one descriptor set per slot serves them all. Order: the header comment in `vulkan_cloth.cpp`.
- **Push constants** (36 bytes): tick, substep, range, level, point, edge and hinge counts, `bendingSubsteps`.
- `clothDispatchesPerTick`: XPBD = substeps (ends + stretch colours) + bending solves times bending colours; vertex block descent = substeps (ends + sweep colours + levels' work), ends being 2, or 3 with stitches. Each dispatch costs about the same fixed few microseconds whatever its size, so dispatch count is the cost.

## A point's lanes

`cloth_point_lanes.glsl`. Summing a point's ~26 terms (inertia, about 6 edges, about 18 hinge corners, the hand) one after another left each read waiting on the last, with too few points a colour to hide it.

- Each point gets `CLOTH_POINT_LANES` invocations of one subgroup (`clothPointLane`, from the subgroup's own numbering, since Vulkan does not promise how a workgroup splits), each adding every eighth term.
- `clothLaneSum`: a butterfly of `subgroupShuffleXor`; paired lanes add the same two numbers, so all end with the same bits in a fixed order.
- Used by the sweep and the levels' gradient (`clothPointForceLanes`).
- Needs shuffles in compute and subgroups of 8 to 64 (`checkPointLanes`, `VulkanDevice::getSubgroupProperties`), or the cloth pipelines throw.

## The coarse levels

- `packClothLevels` flattens a `ClothHierarchy`: every level's points, masses, neighbours, gather columns and spread rows one after another, one buffer each, starts counted from the buffer's front; a `GpuClothLevel` per level says where its own begin. A pass is told only its level's number.
- Uploaded once (none for a cloth without levels) beside the work arrays (gradient, direction, coarse steps, held points, line search partials and step): sixteen buffers at set 1 (`cloth_levels.glsl`). The level pipelines share set 0's layout (`ComputePipelineDescription::sharedSets`).
- `clothLevelsUsed`: vertex block descent only, as many as asked up to those the cloth has. `clothDispatchesPerLevel`: 6.
- After the first sweep: `cloth_held`, then per level, coarsest first, `cloth_gradient`, `cloth_coarse_step` (`clothCoarseEdgeTerm`, `clothBlockSolve`), `cloth_spread`, `cloth_line_terms` (per point, edge and hinge; each workgroup summed by `clothGroupSum`, a fixed-order tree in shared memory), `cloth_line_total` (one workgroup; `clothLineSearchStep` writes the step to a buffer), `cloth_line_apply`; then the second sweep.
- The CPU never reads the step, so a tick is one unbroken string of dispatches.
- Float sums where the CPU uses double: the curvature's terms are never negative, so a tree's error stays small; a near-zero slope gives a harmless tiny step.

## Field bodies

`VulkanClothFields`: every field any cloth has met, one after another in one buffer shared by all cloths (fields are large and the same for every cloth). Set 0 binding 17; the slot's `GpuClothField` records at 16.

- `hold` takes the frame's fields before any cloth records; a new field waits for the GPU and rebuilds the buffer with every field so far (rare: a field appears when its body is made). `recordTicks` rewrites a slot's set when its buffer changed.
- `clothFieldSurfacePoint` (`cloth_pass.glsl`) blends a cell's eight corners with the library's functions.
- A cloth with no field bodies binds an empty stand-in.

## VulkanCloth

One cloth on the GPU (`render/vulkan/vulkan_cloth`).

- **Uploaded once:** points, coloured constraints, pin slots, per-point lists, points in colour order (`ClothVertexColours::order`), tethers, stitches and partners, levels; a prediction buffer beside them.
- **Slots:** one per frame in flight, each with upload buffers for ticks, pins, bodies and field records (grown to powers of two), a descriptor set, and a read-back buffer. One `recordTicks` per slot per submission.
- `recordTicks`: writes the records, keeps the points before the last tick (`keepTickStart`) for drawing between ticks, records every tick's passes. Throws for a bad slot, the wrong number of pin targets, a hand on a missing point, or field bodies not held.
- `recordReadback`: copies points and velocities; `readPositions`, `readVelocities` once the slot's commands finish.
- `attachShape`, `recordShape`: the deforming mesh (below).

## The shape pass

`cloth_shape.comp` with `deform.glsl`, mirrored by `deformVertexAt` (`scene/deforming_mesh`).

- One invocation per render vertex: its point blended by alpha between the points before the last tick and now; normals summed over its `Mesh` vertex's triangles and tangents over its own (`DeformingAdjacency`, compressed rows in triangle order, as `deformVertices` adds them), so the result is `deformVertices`' to the bit.
- Writes `MESH_VERTEX_FLOATS` floats a vertex into the mesh's slot; one barrier makes it readable by drawing, the velocity pass, acceleration structure builds and ray hits.

## Timing

- `ClothPassStamps`: a timestamp pool per frame in flight; `begin` before the frame's first cloth pass, `mark` after each with its `ClothPass` kind, `read` once the fence passes. Only while the game shows its debug view (`VulkanRenderer::setClothPassTiming`) and the device has timestamps.
- `clothPassTimingsFromTicks`: the stamps into seconds per `ClothPass`, each gap credited to the pass it follows. A frame of more passes than `maxClothPassStamps` is marked overflowed, not measured in part.
- Reaches `RenderStats::cloth`; F4's cloth line (`engine/debug_views`: `makeClothProfiler`, `recordClothTimings`) and the `stats` command show it.

## The engine side

`engine/gpu_cloth`. `EngineSettings::clothOnGpu` (on, live).

1. **Choosing:** `Application` calls `Game::useClothSolver` each frame; until then a game is on the CPU, so headless runs, tests and golden captures stay there.
2. **Queueing:** `stepCloths(world, seconds, jobs, ClothSolver::Gpu)` marks each cloth `onGpu` and appends `clothTickCommands` to `Cloth::gpuTicks` instead of stepping. Back on the CPU, both are cleared and the cloth steps from its points.
3. **Handing over:** before `render`, `takeClothWork` moves each GPU cloth's ticks into a `ClothWork` (key `clothKey(entity)`: slot and generation, so a recycled slot is a new cloth; the cloth; its `DeformingMesh`'s mesh and topology), given to `VulkanRenderer::setClothWork(work, alpha)`. `FrameDescription` cannot carry it: `scene/` is below `cloth/`.
4. **In the renderer:** after `showShapes` (which advances a mesh's slot, `VulkanMesh::advanceGpuShape`, only when a cloth's work names it), `runCloths` records each cloth's ticks into its frame's slot (one `VulkanCloth` per key), the shape pass, then the read-back copy. A cloth seen first, or whose points, pins or mesh changed, is uploaded from its CPU state; unlisted cloths are dropped; both wait for the GPU (rare).
5. **Reading back late:** when a frame's fence passes, `collectClothReadbacks` turns its copies into `ClothReadback`s; after `render`, `applyClothReadbacks` writes positions and velocities into each cloth still on the GPU and both ticks of its `DeformingMesh`. Picking, grabbing and debug lines then work on points about two frames old. Readbacks for a cloth gone, back on the CPU or resized are ignored.
6. **The frame's entry:** `gpuClothDeformedMesh` gives no vertices, `shapedOnGpu`, bounds by `lateClothBounds` (the late points' box grown by the fastest point's speed over frames in flight + 1 frames, plus the thickness), and a fingerprint that changes every frame (the CPU cannot tell whether the points moved), so its shadow tiles redraw every frame.
7. **The tick budget:** each GPU tick is charged `gpuClothTickSeconds` (`engine-core.md`).

- **Determinism:** `hashSimulationState` leaves out a GPU cloth's points and mesh shape but keeps its pins and hand.

## Tests

| Test | Checks |
|---|---|
| `cloth_gpu_test.cpp` (in `tests/`) | Packing, record layouts, dispatch counts, the queue |
| `gpu_cloth_engine_test.cpp` | Keys, work, readbacks, late bounds, the solver switch |
| `cloth_timing_test.cpp` | Stamps into per-pass seconds |
| `tests/gpu/cloth_gpu_test.cpp` (`make gputest`) | Every `cloth.glsl` function against its CPU mirror; each of a busy sheet's ticks from the CPU's state within microns of the CPU's next; whole runs twice bit for bit; settled sheets' yardstick measures against the CPU's; the shape pass |

- Whole busy runs are never compared point by point: cloth is chaotic, and a micron's nudge grows to centimetres within a fraction of a second.
