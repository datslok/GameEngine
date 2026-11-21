# Build log: Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards)

## Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards)

- `clusters` (Nov 21 2026): stage 1, meshes cut into clusters culled one by one. The user chose mesh shaders over a compute-written index buffer (published numbers: Granite, RTX 3070, 3.3 ms against 3.9; both of the user's GPUs have `VK_EXT_mesh_shader`).
  - **1a, clusters built** (`scene/mesh_clusters`, meshoptimizer 0.22's builder): 64 vertices and 124 triangles at most, a sphere and a cone of facing each; the cone test is exact under any transform that does not mirror, so the GPU tests the eye brought into the mesh's space.
  - **Cone use measured:** a 48 x 24 sphere's clusters span about 60 degrees, wide cones, about a quarter culled from afar; a 192 x 96 sphere's 389 clusters 47% (half the surface faces away, so 50% is the most).
  - **1b, clusters culled on the GPU** (`render/gpu/mesh_cluster_culling`, four passes after the camera's culling, behind Passes "Mesh clusters", off until 1c draws the list): pairs (instance, cluster) by prefix sums in drawing order, each style from a 256-pair block, the test run twice (count, then write) rather than a flag kept a pair, which could take tens of MB.
  - **Found while building:** the camera culls with no velocity list and no sway (the main pass writes its own motion), so one list, not two; and the cluster passes need their own copy of the view with the wind's sway, or swaying trees would be culled by their rest spheres.
  - **Rigid draws only** are tested: sway leans a tree up to about half a radian and normals are not turned, so a swaying draw near the camera, terrain patches, impostor cards, ground-following plants, lianas and thinned cards keep every cluster.
  - **Merged on the way:** two private copies of `ensureBuffer` (camera culling, placed drawing) became one in `vulkan_memory`; the two places building a `GpuCullGroup` became `placedCullGroup`.
