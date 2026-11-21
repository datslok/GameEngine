# Mesh clusters

Covers: `src/scene/mesh_clusters`, `src/render/gpu/mesh_cluster_culling`, `src/render/gpu/mesh_cluster_culling_shared.h`, `assets/shaders/mesh_cluster_culling.glsl`, `assets/shaders/mesh_cluster_pass.glsl`, `assets/shaders/mesh_cluster_count.comp`, `assets/shaders/mesh_cluster_test.comp`, `assets/shaders/mesh_cluster_scan.comp`, `assets/shaders/mesh_cluster_write.comp`, `src/render/vulkan/vulkan_mesh_cluster_culling`, `src/scene/mesh_clusters_shared.h`, `assets/shaders/mesh_cluster_draw.glsl`, `assets/shaders/triangle.mesh`, `assets/shaders/triangle_motion.mesh`

Meshes cut into small pieces culled one by one (ROADMAP phase 21, stage 1; research `docs/research/VIRTUALGEOMETRY.md`). Not the lights' clusters (`clustered-shading.md`). Cost grows with the camera's drawn instances times their clusters, not with the scene.

## Building (`scene/mesh_clusters`)

`buildMeshClusters` (meshoptimizer's builder):

| Piece | What |
|---|---|
| `MeshCluster` | at most `MESH_CLUSTER_MAX_VERTICES` 64 and `MESH_CLUSTER_MAX_TRIANGLES` 124 (a mesh-shader workgroup's output); a sphere holding its vertices; a cone of facing (apex, axis, cutoff) |
| `MeshClusters` | the clusters; their vertices as the mesh's vertex numbers; their triangles as three local corner bytes, runs packed end to end |

- From triangles near each other and of like facing (cone weight 0.25), each triangle in exactly one with its winding, corners reordered for vertex reuse; deterministic.
- `clusterFacesAway(cluster, eye)`: every triangle's back faces the eye (eye in the mesh's space; exact under any transform that does not mirror, since which side of a triangle the eye is on is the sign of a volume). A cutoff of 1 or more (facings spread past a half turn) never culls.

## In the pools (`render/gpu/mesh_cluster_culling`, `VulkanMeshPool`)

- Every pooled mesh's clusters built as it is added (`VulkanMeshPool::addClusters`, timed: `getClusterBuildSeconds`), packed by `packMeshClusters` into three more pools grown as the others are: `GpuMeshCluster` records (64 B: sphere, cone, axis, runs, counts, its mesh's first vertex), vertex numbers (the vertex pool's own, so no offset is needed), triangle words (four corner bytes a word, each cluster's from a word).
- `MeshPlacement::firstCluster`, `clusterCount`; `GpuCullGroup` carries them (`placedCullGroup`), so the GPU finds a group's clusters.
- The records are kept on the CPU too (`getClusterRecords`), for `cluster-compare`; F4's MESHES counts the cluster pools.

## Culling (`engine.render` Passes "Mesh clusters", off by default until occlusion (stage 1d) makes it pay)

After the camera's culling (`gpu-driven.md`), on its unpacked commands and draw list, as (draw list entry, cluster) pairs (`GpuMeshClusterItem`, 8 B):

| Pass | What |
|---|---|
| `mesh_cluster_count` | one workgroup walks the drawing order's runs style by style: each place's end is a prefix sum of instances x clusters; each style from a block's first pair; the dispatch's blocks (`meshClusterWorkEnds`) |
| `mesh_cluster_test` | indirect, a thread a pair: its place by binary search (`meshClusterPairAt`), the test, each block's survivors |
| `mesh_cluster_scan` | one workgroup: blocks' first places, totals (`MESH_CLUSTER_TOTAL_*`), each style's run cut to the room |
| `mesh_cluster_write` | indirect: the test again (cheaper than a flag a pair), each survivor at its block's first place plus those before it; the written ones' triangles summed |

- **The test** (`meshClusterKept`, mirrored in `mesh_cluster_culling.glsl`): a draw not rigid keeps every cluster (`meshClusterRigid`: terrain patches, impostor cards, ground-following, lianas, cards thinned in this view, swaying within the sway's reach); else the sphere (centre placed, radius by the longest column) in the frustum, then, single-sided and not mirrored, not facing away (the eye brought into the mesh by the inverse's cross products).
- **Its own view:** the camera culls without sway, so the passes get a copy of its placed view (`VulkanCameraCulling::placedView`) with the wind's sway switch and fade end.
- **Fixed order:** places are prefix sums over the drawing order, survivors in it; no atomics decide places (the triangle sum only adds).
- **Sizes:** block sums by `meshClusterPairBound` (each group's candidates x its clusters, plus padding); the list's room from `smallestMeshClusterRoom` (1M pairs), grown to the next power of two after a frame overflows (that frame's list cut).
- **F4:** `CLUSTERS kept/tested trianglesK` on the levels line while on; MESHCULL in the GPU timings.
- **`cluster-compare`:** the next frame's list read back, with the camera culling's commands and entries of the same frame, against `cullMeshClustersOnCpu` over copies of the view, groups, order, records and instances (`compareMeshClusterLists`, as sets; pairs near an edge, `nearAMeshClusterEdge`, may differ).

## Drawing (mesh shaders, `VK_EXT_mesh_shader`)

The same switch draws the list: the camera's prepass and main pass draw each style's pooled groups as its run of surviving clusters, one `vkCmdDrawMeshTasksIndirectEXT` a style, instead of one indexed multi-draw a style. Everything else (draws not pooled: deforming meshes, cloth; captures; shadows) draws as before.

| Piece | What |
|---|---|
| Device (`VulkanDevice::supportsMeshShaders`) | optional extension; used only if a workgroup takes 64 vertices, 124 triangles, 32 threads, and a style's largest run (`largestMeshClusterRoom`, 2^22) fits one draw's workgroups. Without it the list is culled (F4, `cluster-compare`) but not drawn |
| Scan pass | writes each style's `VkDrawMeshTasksIndirectCommandEXT` from its run's count (`meshClusterTaskGroups`: rows of `MESH_CLUSTER_TASK_WIDTH` 32768, since a dimension is only sure to take 65535) |
| Buffers | by address (`buffer_reference`): `GpuFrameData::meshClusterBuffers` holds the list, totals, cluster records, vertex numbers, triangle words and vertex pool (`MeshClusterBuffers`, `packMeshClusterBuffers`); `VulkanMeshClusterCulling::prepare` makes the list ready before the frame's uniform block is written |
| `mesh_cluster_draw.glsl` | one 32-thread workgroup a survivor: its place in the style's run (`meshClusterTaskIndex`; past the run's count, nothing), each vertex placed by `shadeVertex` (`triangle_vertex.glsl`, the vertex shaders' own body), triangles from the corner bytes. `triangle.mesh` (prepass, main), `triangle_motion.mesh` (main with motion vectors) |
| Push constants | the style drawn, at `MESH_CLUSTER_PUSH_OFFSET` 112 (after the shadow and capture views), mesh stage only (`SceneLayout(meshShaders)`) |
| Pipelines | `PipelineDescription::meshShader`; each prepass and main pipeline gets a mesh twin with the same fragment shader (`depthPrepass*MeshPipeline`, `mainMeshPipelines`); `drawPassDraws` uses them when `PassDraws::meshTasks` is set |

- **Same depth bit for bit:** the main pass shades where depth equals the prepass's, so `gl_Position` is `invariant` in both, as in the vertex shaders.
- **One vertex body:** `triangle_vertex.glsl`'s `shadeVertex` writes each output once through `VERTEX_OUT` macros (mesh shaders may not read their outputs back), so vertex and mesh shaders place and sway alike. A pooled mesh never deforms, so last frame's shape is its own.

## Tests

`mesh_clusters_test` (every triangle once with its winding, limits, spheres hold their vertices, facing never culls a front, a fine sphere 47% culled from afar); `mesh_cluster_culling_test` (packing, a culled cluster hides nothing seen over random turned, uneven, mirrored and double-sided instances, draws not rigid keep everything, the passes' steps equal a plain walk with the room cut, the size bound, the mesh shaders' workgroups reaching each survivor once and their buffer addresses, the comparison, the command, F4's line); GPU: `mesh_cluster_culling_gpu_test` (the test's choices, sphere and scale against the mirror).
