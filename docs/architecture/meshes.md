# Meshes

Covers: `src/scene/mesh`, `src/scene/indexed_mesh`, `src/scene/smooth_normals`, `src/scene/tangents`, `src/scene/deforming_mesh`, `src/scene/deforming_mesh_shared.h`, `src/render/gpu/deforming_slots`, `assets/shaders/deform.glsl`

The import format, the render format, normals and tangents, and meshes whose vertices move. Loading and handles: [assets.md](assets.md). UV origin rules: `src/assets/CLAUDE.md`.

## Mesh: the import format

`Mesh` (`scene/mesh.h`): what loaders, code, bounds, collision and the software renderer use.

- `vertices` (`Vec4`, w must be 1), `edges`, `triangles`, `uvOrigin` (`BottomLeft` for OBJ and code, `TopLeft` for glTF).
- Each `Triangle` holds three vertex indices and per-corner optional normal, UV and tangent; an empty normal means the face normal.
- `faceNormal(first, second, third)`: counterclockwise from the front, zero for no area.

### Generated meshes

| Maker | Shape |
|---|---|
| `Mesh::cube()` | unit cube |
| `Mesh::plane(halfSize)` | flat ground, 20 m half size by default |
| `Mesh::sphere(segments, rings)` | unit UV sphere, smooth normals |
| `Mesh::lathe(profile, segments)` | (radius, height) points spun round y (bottles, posts); flat caps where ends are off the axis; smooth along the profile; u round, v up |
| `Mesh::grid(columns, rows, width, height)` | sheet facing +z, centred, rows from the bottom, UV (0, 0) bottom-left: a cloth's rest shape |

Vertex numbering and error cases: the header.

## IndexedMesh: the render format

`IndexedMesh` (`scene/indexed_mesh.h`): unique `MeshVertex`es plus 32-bit indices.

- `MeshVertex`: position, normal, UV, tangent; 48 bytes, no padding, the exact GPU vertex layout (pipelines read it with `offsetof`).
- `resolveCorners(mesh)`: three corners a triangle, unshared, each with its file normal or the face normal, its UV or (0, 0), its tangent. Throws for empty meshes, non-finite positions, w ≠ 1, missing vertices.
- `buildIndexedMesh(mesh)`: `resolveCorners`, then corners identical in every component share a vertex (the key adds +0.0, so -0.0 and +0.0 match).
- Built once, when a mesh first reaches the GPU; `VulkanMesh` only validates (`checkMeshUpload`) and uploads.
- Optional per-vertex records riding along: `hanging` (`HangingVertex`, see `docs/research/WIND.md`), `thinning` and `thinningCards` (`scene/card_thinning.h`). Each list is one entry a vertex or empty.
- `meshPrefix`, `thinnedLevel`: a mesh's first vertices and indices as a level of its own (levels sharing vertices; `docs/research/LOD.md`). `simplifyMesh`, `simplifiedLevels` (`scene/simplify_mesh`, meshoptimizer) make coarser levels.
- `indexedMeshBounds`: the box, as `meshBounds` gives a `Mesh`'s.

## Normals

- Files without normals stay flat shaded (their corners rarely merge), unless loaded with `MeshLoadOptions::smoothNormals`.
- `generateSmoothNormals(mesh, creaseAngle)` (`scene/smooth_normals`): fills only missing corner normals with the average of the faces at that position (grouped by exact position, so seams with duplicated vertices smooth too).
  - Weighted by the corner's angle, so the result follows the surface, not how it was cut into triangles.
  - Only faces within `creaseAngle` (60° by default) of the corner's own face count, so hard edges stay sharp.
  - Opt-in: only the game knows whether a shape is meant faceted.

## Tangents

`MeshVertex::tangent`: xyz the unit direction of increasing u; w ±1, with the bitangent (up the picture) = w · (N × T), so mirrored UVs need no third vector.

- From the file when every corner has one (glTF `TANGENT`).
- Else `generateTangents(corners, origin)` (`scene/tangents`): MikkTSpace (`external/mikktspace`, C99), the baking tools' algorithm, so a normal map is read in the frame it was baked in. `TopLeft` meshes are given (u, 1 − v), so the bitangent points up the picture as Blender's exporter makes it.
- No UVs, or UVs squashed to a line: any unit tangent across the normal, sign +1. MikkTSpace's placeholder for UV-less patches is put across the normal by one Gram-Schmidt step: the shader needs a frame everywhere.
- Mirror seams (same position, normal, UV, opposite sign) stay separate vertices.

## Deforming meshes

Vertices move every tick, triangles and UVs stay: cloth.

### The game's side

- `AssetManager::addDeformingMesh(mesh)`: its own mesh (never shared), its `DeformingTopology` built once (`isDeforming`, `getDeformingTopology`). The `Mesh` is the rest shape.
- Drawn by a `ModelRenderer` part as usual; its entity holds a `DeformingMesh {mesh, positions, previousPositions}`, one position per `Mesh` vertex in the mesh's own space, written in ticks.
- `savePreviousShapes` runs before each tick (with `savePreviousTransforms`); `hashSimulationState` includes the positions.

### Topology

`buildDeformingTopology`: the render vertices (corners sharing a `Mesh` vertex, UV and tangent side merged, as `buildIndexedMesh` does) and `sourceVertex` (which `Mesh` vertex each is).

- Corners split at a UV seam share one normal, so the surface stays smooth across seams.
- `rest`: the rest shape's vertices, worked out by `deformVertices` itself; this is what is uploaded.

### Shaping each frame

In `Application::describeFrame`, one mesh a job (`parallelFor`):

1. `deformMesh(topology, mesh, alpha)`: `blendedPositions` between the last two ticks (the latest when no previous shape of the same size), then `deformVertices`.
2. `deformVertices`, its steps shared with the GPU (`deform.glsl` mirrors them; thresholds in `deforming_mesh_shared.h`):

| Step | What |
|---|---|
| `triangleAreaNormal` | cross product: normal × twice the area, so sums weight by area |
| `triangleTangent` | Lengyel's direction of increasing u × the sign of the UV area; none without UV area |
| `deformedNormal` | summed normal made unit; +y below `DEFORM_SMALLEST_DIRECTION` |
| `deformedTangent` | summed tangent made square to the normal (Gram-Schmidt), unit, side w kept; any perpendicular when none (`DEFORM_PERPENDICULAR_SWITCH` picks the axis) |

3. The `DeformedMesh {mesh, vertices, bounds, shape, shapedOnGpu}` goes into `FrameDescription::deformedMeshes`. `shape` is an FNV-1a fingerprint of the blended positions, never 0. Its bounds replace the mesh's in `boundsByMesh` before culling.
4. `attachShapes` sets `DrawItem::shape` on that mesh's draws built this frame (kept still draws never deform); `shadowCasterSignature` hashes it, so a caster changing shape in place redraws its tiles and a still one keeps them.

- A single big sheet is one job, not split across threads.
- **GPU cloth** (`shapedOnGpu`): `gpuClothDeformedMesh` (`engine/gpu_cloth`) sends no vertices; bounds from the points read back, grown by the fastest point's speed × the frames' lateness plus the thickness; `shape` a per-frame fingerprint. The cloth's shape pass (`cloth_shape.comp`) writes the vertices straight into the mesh's slot, one invocation a render vertex gathering its own triangles; `DeformingAdjacency` and `deformVertexAt` are its CPU mirror, bit for bit `deformVertices`. Cloth itself: `docs/research/CLOTHGPU.md` and the cloth architecture.

### The ring of slots

`DeformingSlots` (`render/gpu/deforming_slots`), `deformingSlotCount` = `maxFramesInFlight` + 1 = 3.

- A deforming `VulkanMesh` (`uploadMesh(rest, true)`) keeps its vertices in three mapped upload buffers (`shapeSlots`), all starting as the rest shape; indices live in GPU memory.
- A frame reads the current slot and the previous one (motion). A new shape goes into the slot after the current (`advance`), and current moves at most one step a frame, so with F frames in flight the slot written is never one frames N − F + 1 .. N − 1 read. No extra fences or copies.
- `VulkanRenderer::showShapes`, first in `drawScene` after the frame's fence: each deforming mesh shows or holds exactly once a frame.
  - `showShape`: upload into the next slot, or hold when the fingerprint matches the shape shown (no upload, no motion).
  - `holdShape`: not in this frame's list.
  - `advanceGpuShape`: a GPU cloth in this frame's cloth work takes the next slot for its shape pass; otherwise it holds.
  - A deformed mesh not uploaded as deforming throws.
- `version(slot)` counts every write, so what was built from a slot knows it is stale.

### Motion vectors

- The main pass works out a vertex's motion when its matrix moved, the wind offsets it, or its shape changed (`anti-aliasing.md`).
- The motion pipelines (`BatchPipelines::previousShapes`) read a second vertex stream (`VertexLayout` attributes naming binding 1) bound to last frame's slot, at location 4 of `triangle_motion.vert`, placed with last frame's matrix. Other meshes bind their one buffer twice.

### Ray tracing

- One bottom-level structure per slot (`MeshStructure::slotStructures`; fast build, not compacted, a scratch buffer each), slot 0 built at upload.
- `rebuildDeformedMeshes`, recorded before each top-level build, rebuilds the current slot's structure when its version is newer than the one it was built from (barrier for the reused scratch first, then one making it readable by the top-level build and ray queries).
- `packInstances` and the mesh table use `currentStructure` and `currentVertices`.

## Tests

`indexed_mesh_test`, `smooth_normals_test`, `tangents_test` (regenerates NormalTangentMirrorTest's exported tangents: at least 99% of corners match, signs included), `sphere_mesh_test`, `lathe_mesh_test`, `simplify_mesh_test`, `deforming_mesh_test`, `deforming_slots_test` (every order of new and held frames).
