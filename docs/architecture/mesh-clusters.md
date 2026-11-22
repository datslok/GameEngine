# Mesh clusters

Covers: `src/scene/mesh_clusters`, `src/render/gpu/mesh_cluster_culling`, `src/render/gpu/mesh_cluster_culling_shared.h`, `assets/shaders/mesh_cluster_culling.glsl`, `assets/shaders/mesh_cluster_pass.glsl`, `assets/shaders/mesh_cluster_count.comp`, `assets/shaders/mesh_cluster_test.comp`, `assets/shaders/mesh_cluster_scan.comp`, `assets/shaders/mesh_cluster_write.comp`, `src/render/vulkan/vulkan_mesh_cluster_culling`, `src/scene/mesh_clusters_shared.h`, `assets/shaders/mesh_cluster_draw.glsl`, `assets/shaders/triangle.mesh`, `assets/shaders/triangle_motion.mesh`

Meshes cut into small pieces culled one by one (ROADMAP phase 21, stage 1; research `docs/research/VIRTUALGEOMETRY.md`). Not the lights' clusters (`clustered-shading.md`). Cost grows with the camera's drawn instances times their clusters, not with the scene.

## Building (`scene/mesh_clusters`)

`buildMeshClusters` (meshoptimizer's `clusterlod`, Nanite's scheme) cuts a mesh into clusters, then builds levels over them (stage 2):

| Piece | What |
|---|---|
| `MeshCluster` | at most `MESH_CLUSTER_MAX_VERTICES` 64 and `MESH_CLUSTER_MAX_TRIANGLES` 124 (a mesh-shader workgroup's output); a sphere holding its vertices; a cone of facing (apex, axis, cutoff); `group` (simplified with), `refinedGroup` (made from; `meshClusterNoGroup` for the mesh's own triangles) |
| `MeshClusterGroup` | about 16 clusters merged and simplified together: the error of what was made from them (mesh units; the largest float when nothing coarser was) and a sphere holding every group under it; its level |
| `MeshClusters` | the clusters, the mesh's own first (`originalCount`), coarser levels after; their vertices as the mesh's vertex numbers (every level's corners are the mesh's own vertices); their triangles as three local corner bytes, runs packed end to end; the groups |

- **Only where used** (`ClusterLevels`, Nanite's per-mesh switch): a game marks a mesh with `AssetManager::setClusterLevels` before it reaches the GPU; the forest marks its rocks, drifting rocks, structures and trees' bark (`withClusterLevels`), not leaves, strips, terrain or the old levels. Unmarked meshes get `ClusterLevels::One`: the same full-detail clusters, cut by `clusterlod`'s own first-level steps (`meshopt_buildMeshletsFlex`, then reordered), all in one group whose error is the largest float, so the cut always draws them.
- **Levels:** neighbouring clusters grouped (fewest shared edges), simplified to half with the group's border locked, cut back into clusters, repeated until one is left or a group will not simplify; borders move level to level, so no seam stays locked. About twice the mesh's triangles in all.
- **Simplification:** normals weighed 0.5 (`clusterlod`'s demo; raised only if levels change shading), UV seams kept (protected), sloppy simplification only when the regular one gets stuck (its error doubled).
- **Given levels** (`buildGivenClusterLevels`, `GivenClusterLevel`; stage 3b-i): levels made by the mesh's maker instead of simplified, for a twig's pruned leaves (`forest-twigs.md`), which simplification would erode. The mesh's vertices hold every level's, its indices the finest; each coarser level is its triangles and its error. They become a chain: group k is level k's clusters (cut as `ClusterLevels::One` cuts), made from group k - 1, its error the next level's (the coarsest the largest float), its sphere round its own clusters, the next level's and group k - 1 (meshoptimizer's sphere, grown to hold each exactly), so every cut draws one whole level. Falling errors, part triangles or corners past the vertices throw. `MeshClusterBuild::given` takes them in a batch; `AssetManager::setGivenClusterLevels` keeps them for drawn-only meshes and marks the mesh.
- **The cut:** a cluster whose group's error is too big and whose refined group's is small enough (`meshClusterInCut`, below). One choice everywhere, since a group's error is at least, and its sphere holds, those of every group under it; watertight, since borders stay locked while a group is simplified.
- **Many at once** (`buildMeshClusters(meshes, jobs)`): a mesh a job, the largest first (so a big mesh does not finish last), results in the order asked, each what building it alone gives. Forest, laptop: 10.3M triangles in 2448 meshes, 27.3 s on one thread, 3.9-4.2 s on the job system.
- Deterministic.
- `clusterFacesAway(cluster, eye)`: every triangle's back faces the eye (eye in the mesh's space; exact under any transform that does not mirror, since which side of a triangle the eye is on is the sign of a volume). A cutoff of 1 or more (facings spread past a half turn) never culls.

## In the pools (`render/gpu/mesh_cluster_culling`, `VulkanMeshPool`)

- Meshes go up in batches (`VulkanRenderer::uploadMeshes`; the loading screen's batches hold up to 1M triangles or 256 meshes): the pooled meshes' clusters built at once on the job system (timed, `getMeshClusterBuildSeconds`, logged after the start-up line), then each mesh added in order with its clusters (`VulkanMeshPool::add`).
- Every level goes into the pools and `MeshPlacement::clusterCount` counts them all (the cut picks). Packed by `packMeshClusters` into three more pools grown as the others are: `GpuMeshCluster` records (112 B: sphere, cone, axis, runs, counts, its mesh's first vertex; the cut's own and parent spheres and errors), vertex numbers (the vertex pool's own, so no offset is needed), triangle words (four corner bytes a word, each cluster's from a word).
- `MeshPlacement::firstCluster`, `clusterCount`; `GpuCullGroup` carries them (`placedCullGroup`), so the GPU finds a group's clusters.
- The records are kept on the CPU too (`getClusterRecords`), for `cluster-compare`; F4's MESHES counts the cluster pools.

## Culling (`engine.render` Passes "Mesh clusters", on)

After the camera's culling (`gpu-driven.md`), on its unpacked commands and draw list, as (draw list entry, cluster) pairs (`GpuMeshClusterItem`, 8 B):

| Pass | What |
|---|---|
| `mesh_cluster_count` | one workgroup walks the drawing order's runs style by style: each place's end is a prefix sum of instances x clusters; each style from a block's first pair; the dispatch's blocks (`meshClusterWorkEnds`) |
| `mesh_cluster_test` | indirect, a thread a pair: its place by binary search (`meshClusterPairAt`), the test, each block's survivors |
| `mesh_cluster_scan` | one workgroup: blocks' first places, totals (`MESH_CLUSTER_TOTAL_*`), each style's run cut to the room |
| `mesh_cluster_write` | indirect: the test again (cheaper than a flag a pair), each survivor at its block's first place plus those before it; the written ones' triangles summed |

- **The test** (`meshClusterKept`, mirrored in `mesh_cluster_culling.glsl`): first the cut, for every draw; then a draw not rigid keeps every cluster (`meshClusterRigid`: terrain patches, impostor cards, ground-following, lianas, cards thinned in this view, swaying within the sway's reach); else the sphere (centre placed, radius by the longest column) in the frustum, then, single-sided and not mirrored, not facing away (the eye brought into the mesh by the inverse's cross products).
- **The cut** (`meshClusterInCut`, phase 21 2c; Nanite's): drawn when its own error (the group it was made from: 0 for the mesh's own triangles) covers at most `engine.render.passes.meshClusterError` pixels (default 1) and its parent's (the group it was simplified with) more, or nothing coarser was made (above `MESH_CLUSTER_NEVER_COARSER`). `meshClusterErrorPixels`: the error times the model's longest stretch times the view's pixels a metre away, over the distance to the group's placed sphere (at least `MESH_CLUSTER_NEAREST_ERROR_DISTANCE`, so 0 stays 0); `precise` in GLSL, so a group's two uses round alike. Unmarked meshes (one level) always pass. Every level's clusters are tested (2d walks the tree only if this shows in the timings).
- **The old levels stand aside** (2e): where the camera draws by mesh shaders, `LevelView::clusterLevels` (`CULL_LEVELS_CLUSTERS`) is set, and a draw whose mesh has cluster levels (`DrawItem::clusterLevels`, `CULL_CLUSTER_LEVELS`, from `AssetManager::getClusterLevels`) keeps level 0 there, a last level drawing nothing included: no old level, no card cutting its trunk, the cut alone coarsening it (`walksLevels`; `levelSlotWithin` and its GLSL mirror). Shadow tiles, captures, the mirror and GPUs without mesh shaders still walk the old levels until stage 4.
- **Placed plants** (2f; `planting.md`): the placed culling runs before these passes, and the order they walk (`meshClusterDrawOrder`) is each style's draw groups, then its placed groups (numbered past the draw groups, `setup.w`; `GpuPlacedGroup` carries its clusters). A pair's place from `PLACED_INSTANCE_BASE` is a placed entry: its instance and record built from plant and part (`meshClusterDrawnAt`, mirrored in `mesh_cluster_pass.glsl`, as `drawnObject` does). The room adds `placedMeshClusterPairBound` (each of the camera's items' plants times its layer's most: every level slot of a kind's parts). Where the camera draws by mesh shaders its placed indexed draws stand aside (`framePlacedByClusters`); every style's mesh draw runs, since a style may hold placed clusters alone.
- **Its own view:** the camera culls without sway, so the passes get a copy of its placed view (`VulkanCameraCulling::placedView`) with the wind's sway switch and fade end, and the cut's pixel error as `levels.pixelError`.
- **Fixed order:** places are prefix sums over the drawing order, survivors in it; no atomics decide places (the triangle sum only adds).
- **Sizes:** block sums by `meshClusterPairBound` (each group's candidates x its clusters, plus padding); the list's room from `smallestMeshClusterRoom` (1M pairs), grown to the next power of two after a frame overflows (that frame's list cut).
- **F4:** `CLUSTERS kept/tested trianglesK` on the levels line while on; MESHCULL in the GPU timings.
- **`cluster-compare`:** the next frame's list read back, with the camera culling's commands and entries and the placed drawing's commands, entries and slots of the same frame (`VulkanPlacedDrawing::readBack`), against `cullMeshClustersOnCpu` over copies of the view, groups, order, records, instances and placed parts (`compareMeshClusterLists`, as sets; pairs near an edge, `nearAMeshClusterEdge`: a plane, the cone, the sway's reach or the cut's pixel error, may differ).

## Occlusion (two passes, Passes "Mesh cluster occlusion", on; research `docs/research/OCCLUSION.md`)

Needs the clusters drawn by mesh shaders and the depth prepass. Two lists, each its own run of the items (room apart), totals (`MESH_CLUSTER_TOTAL_WORDS` apart, with `LIST_FIRST`) and task commands; every pass pushes the list it makes.

| Step | When | What |
|---|---|---|
| Early list | `cull`, before the prepass | the four passes; a rigid cluster in the frustum and facing (`MESH_CLUSTER_SPHERE`) kept unless last frame's pyramid hides it |
| Prepass | | the early list and everything else |
| `cullLate` (MESHLATE) | after it | this frame's pyramid from that depth; test, scan and write again over the same work: kept if last frame's pyramid hid it (a candidate) and this one does not |
| Late prepass (LATEDEPTH) | | the late list alone, depth and solid mask loaded |
| `keepDepth` (HIZ) | | the whole depth into the history pyramid for the next frame |
| Main pass | | both lists |

- **The test** (`meshClusterHidden`; the pass's `meshClusterHiddenIn` reads the pyramid): the sphere, measured from the camera, as its box's eight corners through a camera-relative view-projection (`meshClusterScreenBox`: pixels least and most, nearest depth; none if a corner is within `MESH_CLUSTER_SMALLEST_W` of the camera's plane); the pixels clamped to the screen (`meshClusterBoxPixels`); the level where they span two cells at most (`meshClusterPyramidLevel`); hidden if every cell is farther than the nearest corner.
- **Pyramids** (`VulkanDepthPyramid`, farthest depth, render size, made at the first cull): the history and this frame's. The late list's candidates need the history, so it outlives this frame's build.
- **History:** kept with the jittered view-projection and camera it was drawn with; the early list reads it through `previousRelativeViewProjection` only when the cull just before kept it (a frame without occlusion, or a resize, drops it: the early list keeps everything, the late one is empty).
- **Nothing pops:** whatever the early list wrongly hides (a turn, a mover, no history) the late list catches, since this frame's partial depth is never nearer than its whole depth. Movers are tested at their current place against last frame's depth: a wrong guess costs a late draw, never a missing one.
- **F4:** `LATE` (the late list's survivors) and `HIDDEN` (candidates it left out) after `CLUSTERS`; `cluster-compare` takes both lists together, which may lack the CPU's pairs (hidden) but never add any.

## Drawing (mesh shaders, `VK_EXT_mesh_shader`)

The same switch draws the list: the camera's prepass and main pass draw each style's pooled groups as its run of surviving clusters, one `vkCmdDrawMeshTasksIndirectEXT` a style, instead of one indexed multi-draw a style. Everything else (draws not pooled: deforming meshes, cloth; captures; shadows) draws as before.

| Piece | What |
|---|---|
| Device (`VulkanDevice::supportsMeshShaders`) | optional extension; used only if a workgroup takes 64 vertices, 124 triangles, 32 threads, and a style's largest run (`largestMeshClusterRoom`, 2^22) fits one draw's workgroups. Without it the list is culled (F4, `cluster-compare`) but not drawn |
| Scan pass | writes each list's and style's `VkDrawMeshTasksIndirectCommandEXT` from its run's count (`meshClusterTaskGroups`: rows of `MESH_CLUSTER_TASK_WIDTH` 32768, since a dimension is only sure to take 65535) |
| Buffers | by address (`buffer_reference`): `GpuFrameData::meshClusterBuffers` holds the list, totals, cluster records, vertex numbers, triangle words and vertex pool (`MeshClusterBuffers`, `packMeshClusterBuffers`); `VulkanMeshClusterCulling::prepare` makes the list ready before the frame's uniform block is written |
| `mesh_cluster_draw.glsl` | one 32-thread workgroup a survivor: its place in the style's run (`meshClusterTaskIndex`; past the run's count, nothing), each vertex placed by `shadeVertex` (`triangle_vertex.glsl`, the vertex shaders' own body), triangles from the corner bytes. `triangle.mesh` (prepass, main), `triangle_motion.mesh` (main with motion vectors) |
| Push constants | the style drawn and its list, at `MESH_CLUSTER_PUSH_OFFSET` 112 (after the shadow and capture views), mesh stage only (`SceneLayout(meshShaders)`) |
| Pipelines | `PipelineDescription::meshShader`; each prepass and main pipeline gets a mesh twin with the same fragment shader (`depthPrepass*MeshPipeline`, `mainMeshPipelines`); `drawPassDraws` uses them when `PassDraws::meshTasks` is set |

- **Same depth bit for bit:** the main pass shades where depth equals the prepass's, so `gl_Position` is `invariant` in both, as in the vertex shaders.
- **One vertex body:** `triangle_vertex.glsl`'s `shadeVertex` writes each output once through `VERTEX_OUT` macros (mesh shaders may not read their outputs back), so vertex and mesh shaders place and sway alike. A pooled mesh never deforms, so last frame's shape is its own.

## Twigs

A plant's twigs drawn as instances of prototypes, culled a brick, then a twig, before their clusters, which take them as groups: `twig-drawing.md`.

## Tests

`mesh_clusters_test` (every triangle once with its winding, limits, spheres hold their vertices, facing never culls a front, a fine sphere over 40% culled from afar); `mesh_cluster_levels_test` (the hierarchy's shape, one level the same full-detail clusters in one group, a group bounding every group under it, every cut closed, uniform and seen from eyes, the sphere's volume within its area times the cut's error, an open sheet's cuts open only on its border, one cluster's mesh one level, repeatable, many built at once as one by one); `mesh_cluster_culling_test` (packing, a culled cluster hides nothing seen over random turned, uneven, mirrored and double-sided instances, draws not rigid keep everything in their cut, the cut's records, the cut closed and within its error of the volume from inside to far away, coarser as the eye backs away, the mesh's own clusters with no error allowed and the coarsest far off, one-level meshes whole, the passes' steps equal a plain walk with the room cut, the size bound, the order taking the placed groups, placed plants culled as their entities alone and among the draws, the placed bound, the mesh shaders' workgroups reaching each survivor once and their buffer addresses, the screen box holding its sphere, occlusion hiding nothing with a nearer pixel on odd and thin screens, the two lists disjoint and missing only what this frame's pyramid hides, the occlusion record, the comparison with and without occlusion, the command, F4's line); GPU: `mesh_cluster_culling_gpu_test` (the test's choices, sphere and scale, the cut and its errors' pixels, the screen box, pixels and level against the mirror).
