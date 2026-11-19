# Terrain and ground following

Covers: `src/scene/terrain`, `src/scene/terrain_heights`, `src/scene/terrain_shared.h`, `src/scene/ground_follow`, `src/render/gpu/terrain_gpu`, `assets/shaders/terrain.glsl`, `assets/shaders/terrain_heights.glsl`, `assets/shaders/terrain_data.glsl`

Land drawn as a quadtree of grid patches round the camera (CDLOD, Strugar 2010), and flat meshes lifted onto it by the vertex shaders. Research: `docs/research/LOD.md`, Terrain; `docs/research/GROUNDCOVER.md`. Where it sits in frame building (TERRAIN, ATTACH): [game-interface.md](game-interface.md). The forest's land: [forest.md](forest.md).

## Heights (`scene/terrain_heights`)

Every function here is mirrored in `terrain.glsl` / `terrain_heights.glsl` / `terrain_data.glsl`, so the patches the GPU draws are tested on the CPU. The heights' reads (`terrain_heights.glsl`, the scene set's binding 35 unless the includer names another) multiply by each grid's reciprocal spacing made on the CPU (`GpuTerrainGrid::inverseSpacing`) and are `precise`, so `terrainSurfaceHeight` is the CPU's bit for bit (the planting cells stand plants on it).

- `TerrainHeightGrid`: corner (least x, z), spacing, `size` points a side, heights row by row along x. `makeTerrainHeightGrid(centre, halfSize, spacing, height, jobs)` samples a function, a row a job.
- `TerrainHeights`: up to `TERRAIN_MOST_GRIDS` (4) grids, finest first; a place reads the finest holding it, past all of them the coarsest's nearest edge. A place's grid coordinate is times the spacing's reciprocal (`plantingCoordinate`), so the planting reads the ground the same on the GPU (`planting.md`).

| Function | Gives |
|---|---|
| `terrainHeight` | Bilinear height (`terrainBilinear`) |
| `terrainNormal(heights, x, z, step)` | Unit normal from the slope over `step` either way |
| `terrainSurfaceHeight` | The height as the finest patches draw it: each grid square two flat triangles split from its near right corner to its far left (`terrainTriangleHeight`), as `terrainGridMesh` splits it |
| `terrainHeightRange(heights, west, east, north, south)` | Every height either read gives in a box: the grid points round its squares, finest grid up to the first holding the whole box, a hair wider for rounding (CPU only; the placed plants' cell boxes) |
| `terrainMorph(distance, start, end)` | 0 before start, 1 from end, linear between |
| `morphGridCoordinate(grid, morph)` | Odd grid coordinates slide towards their even neighbour below; at 1 the patch is the next coarser grid |
| `terrainVertex(heights, patch, meshPoint, eye)` | A patch vertex: below |

## The `Terrain` component (`scene/terrain`)

A game places one on an entity without a `Transform` (heights are where they are). Fields in `scene/terrain.h`:

- `heights` and `heightsVersion` (bump on any change: the renderer re-uploads).
- `bounds`: `makeTerrainBounds(terrain, jobs)`, each node's lowest and highest height, leaves (every grid point and corner) then parents; needed by selection.
- The quadtree: `centre`, `leafSize`, `levels`; the root is leafSize · 2^(levels - 1) across.
- `landHalfSize`: nodes wholly outside it are left out.
- `regions` (`TerrainRegion`: a square and its `Material`, weights texture across it): a node takes the finest region holding it whole, else the last. At least one is required.
- `grid`, `halfGrid`: `terrainGridMesh(TERRAIN_GRID_QUADS)` (32) and `terrainGridMesh(TERRAIN_HALF_GRID_QUADS)` (16), flat 0..1 in x and z, facing up, added by the game.

## Choosing nodes

`selectTerrainNodes(terrain, eye, settings)`, called before `buildFrame` so the frame reserves room (`laterDraws`):

- A level reaches `terrainRange(level)` = `firstRangeMetres` · 2^level; "reaches" is the eye's distance to the node's 3D box (its height bounds included).
- From the root down: a node its own level does not reach is left to its parent, which draws that quarter at the parent's level on the **half grid** (same spacing); a node the next finer level reaches is split; the rest are drawn whole. The root is always drawn or split; level 0 is never split.
- Every place lies in exactly one node, and neighbours differ by at most a level (tested).
- `terrainNodeAt`: a node's spacing (size / quads) and morph band: from `end - morphShare · (end - previous range)` to `end` = its level's range.

## The draws

`appendTerrainDraws(terrain, nodes, draws, patches)`, a draw per node:

- Mesh `grid` or `halfGrid`; model matrix the corner and size across x and z (for culling and the shadow cache); `previousModel` the same (no motion).
- Bounds: the sphere round the node's box from its height bounds. Casts shadows; not static (the static level is the game's own hidden mesh).
- The region's material; `DrawItem::terrainPatch` indexes `FrameDescription::terrainPatches`.
- `TerrainPatch`: corner, size, quads, spacing, morph start and end (metres from the eye), the region's UV corner and UV per metre.
- No ray meets a patch (`rayMaskFor`): its mesh is flat on the CPU.

## The vertex shaders

- `packInstance` writes the patch index plus one in `GpuInstance::material[2]` (0: not terrain).
- `triangle.vert` (prepass, main pass, probe faces, the mirror) and `shadow.vert` build the vertex with `terrainVertex` (`terrain_data.glsl`):
  1. The grid point on the node; its 3D distance from the camera at the land's height there gives the morph.
  2. Slid by the morph; lifted to the land's bilinear height where it lands.
  3. Normal from the slope over the spacing, growing to the coarser level's spacing as it morphs.
  4. Tangent along x (u grows with x), bitangent sign -1 (v grows with z); UVs across the region's square.
- The morph is always from the **camera**, also in shadow tiles, so shadows fall from the surface seen.

## GPU records (`render/gpu/terrain_gpu`)

| Record | Binding (scene set 1) | Size | Written |
|---|---|---|---|
| `GpuTerrainPatch` (placement, morph, uv) | 36 | 48 | Each frame (`packTerrainPatch`), at least one record |
| `GpuTerrainHeightsHeader` (count, `GpuTerrainGrid` x 4) then every grid's heights | 35 | 144 + heights | `packTerrainHeights` (throws past 4 grids) |

- `VulkanRenderer::updateTerrainHeights`: re-uploads through a staging buffer only when the frame's heights pointer or `terrainHeightsVersion` changes. Without a terrain a header-only stand-in is bound (no grids, so nothing is lifted).
- Bindings and their stages: [gpu-scene.md](gpu-scene.md).

## Settings

`TerrainSettings` (`RenderSettings::terrain`, F5 Terrain, `engine.render.terrain.*`), live: `firstRangeMetres` (200), `morphShare` (0.3). `validateTerrain` rejects a range not above 0 or a share outside (0, 1].

## Ground following (`scene/ground_follow`)

Grass and ground cover built flat on y = 0 of their own frame, lifted onto the land per vertex, so a few shared patches serve every lumpy spot instead of a mesh each.

- **The flag:** `ModelRenderer::followsGround` → `DrawItem::followsGround` → `INSTANCE_KIND_FOLLOWS_GROUND` in `GpuInstance::material[3]` (an impostor stays an impostor).
- **Shaders:** every instance-reading vertex shader (`triangle.vert`, `shadow.vert`, `shadow_cut_out.vert`, `glass_shadow.vert`, `motion.vert` for both frames) adds `groundLift(kind, world)`: `terrainSurfaceHeight` at the vertex's own world place, after card thinning and before wind sway.
- Why the drawn triangles, not the bilinear height: on lumpy land the two differ by centimetres, enough to bury fallen leaves. Over a blade's lean the lift is its root's height. Normals stay the mesh's own.

**On the CPU**, which never moves vertices, only what it reasons with:

- `liftedOntoGround(heights, point)`: the shaders' lift.
- `liftedSphere(sphere, heights)`: the drawn land's lowest and highest under the sphere's square, read at its corners, at every finest-grid point inside it, and along its edges wherever they cross a grid line or a square's diagonal (the triangles' extremes lie there; at most 129 lines each way, evenly spaced past that). Moves the sphere up by their middle, grows it by half their range; unknown radius stays unknown.
- `liftGroundDraws(draws, heights)`: lifts `bounds` and `levelBounds` of following draws, once, in the ATTACH step (kept still draws lifted once when built). No heights: nothing moves.
- No ray meets them (`rayMaskFor`): a ray would meet the unlifted mesh.

## Hidden ground

A terrain's patches are drawn but never collided with. The ground walked on and baked against is a separate mesh with `ModelRenderer::hidden` (no view, shadow or ray, still a sky blocker when static: `blocksSky`); [game-interface.md](game-interface.md).

## Tests

`terrain_test` (selection covers the land, neighbours a level apart, morphed edges meet), `ground_follow_test`; GPU: `terrain_gpu_test`.
