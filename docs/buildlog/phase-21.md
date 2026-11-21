# Build log: Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards)

## Phase 21: Virtual geometry and virtual shadow maps (Nov 21 2026 onwards)

- `clusters` (Nov 21 2026): stage 1, meshes cut into clusters culled one by one. The user chose mesh shaders over a compute-written index buffer (published numbers: Granite, RTX 3070, 3.3 ms against 3.9; both of the user's GPUs have `VK_EXT_mesh_shader`).
  - **1a, clusters built** (`scene/mesh_clusters`, meshoptimizer 0.22's builder): 64 vertices and 124 triangles at most, a sphere and a cone of facing each; the cone test is exact under any transform that does not mirror, so the GPU tests the eye brought into the mesh's space.
  - **Cone use measured:** a 48 x 24 sphere's clusters span about 60 degrees, wide cones, about a quarter culled from afar; a 192 x 96 sphere's 389 clusters 47% (half the surface faces away, so 50% is the most).
