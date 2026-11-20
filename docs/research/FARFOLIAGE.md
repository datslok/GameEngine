# Far foliage as volumes: research

Whether Unreal 5.7's Nanite Foliage, or a smaller version of it, suits this engine's far trees (phase 18, after `VALLEY.md`'s "Drawing the whole valley"). [V] read, [S] secondhand or inferred. Several papers' own timing tables could not be fetched.

## Nanite Foliage (Unreal 5.7, experimental)

Source: [Epic, Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage) [V] unless marked.

| Part | What | Numbers |
|---|---|---|
| Assemblies | a tree as a hierarchy of instanced parts, transforms made while culling clusters; no geometry duplicated | up to 65k part instances; Witcher 4's largest tree 3.5 GB to 29 MB on disk, one view's streaming 36 to 2.7 MB; an example tree 41M triangles from 12 parts, 2,160 instances, 850 bones; 500k tree instances in the demo |
| Voxels | clusters of at most 128 bricks of 4x4x4 voxels about a pixel each; a cluster turns to voxels at a level only where that has the lower error (formula unpublished); opacity from random rays through each voxel, the hit share thresholded to 0 or 1 ([forum](https://forums.unrealengine.com/t/nanite-voxelization-holes-in-the-mesh/2723578)) | "generally performs better than triangle rasterization at distance" |
| Drawing voxels | binned apart, a raster permutation, front to back in depth buckets; each voxel a distribution of normals, one picked per pixel at random (TAA settles it); bricks look ray-traced per pixel inside Nanite's compute rasteriser ([S], shader names in a [crash report](https://forums.unrealengine.com/t/potential-gpu-crash-in-nanite-voxel-brick-rasterizer-out-of-bounds-groupshared-read-in-micropolyrasterize-naniterasterizer-usf-gpu-device-hung-lds-0xd74/2753987)) | |
| Wind | bones, not vertex offsets (which need padded bounds and a bin a material); bounds from the bones | about 0.1 ms a 100k bones; still below 0.1 of the screen |
| Shadows | virtual shadow maps "require disabling animation at distance"; hardware ray-traced shadows use a polygonal fallback ([forum](https://forums.unrealengine.com/t/bad-raytraced-shadows-with-voxelised-nanite-foliage-in-5-7/2696233)) | |
| Leaves | alpha masks discouraged: geometric leaves | |

- **Known problems [V forums]:** holes in solid surfaces (a surface through a voxel's middle hits about half the rays); trunks must be meshes apart from leaves; flat, noisy shading from the normal distribution.
- **Timings [S]:** a user's 77,376 trees of 20M polygons 62 to 119 fps ([80.lv](https://80.lv/articles/get-a-glimpse-of-nanite-foliage-with-voxel-representation-in-ue5-7)); no per-pass foliage figures, voxel memory a tree or error metric published by Epic or CD Projekt.
- **Ray traced instead [S]:** NVIDIA's Mega Geometry foliage demo, about 1M trees and 60M plants on 5 x 5 km, about 58 fps on an RTX 4070 at 1440p with DLSS Quality, 5 GB of video memory.

## Earlier volume methods

| Method | What | Fit here |
|---|---|---|
| Decaudin & Neyret 2004 [S] | volumetric texture slices tiled over the terrain; about 30 fps on a GeForce FX 5800 | blended slices: fights the prepass, EQUAL and TAA |
| Volumetric billboards 2009 [V abstract] | six slice stacks an object, no sorting | blending again |
| Bruneton & Neyret 2012 [V abstract] | near trees as precomputed z-fields with light fields; far trees as view- and light-dependent shaders on the terrain holding sub-pixel effects, blended seamlessly ([EG](https://diglib.eg.org/handle/10.1111/v31i2pp373-382)) | the far half is a terrain shader: cost with pixels; z-fields large a model, no wind |
| Heitz & Neyret 2012 [V abstract] | sparse voxel octree, a distance and Gaussian sub-voxel detail a voxel, ray-marched | a full ray-marcher |
| Loubet & Neyret 2017 [V abstract] | mesh above a pixel, SGGX microflake volume below, occlusion kept; trees a target | offline; Nanite Voxels' closest ancestor |
| Zhou et al. 2025 [V abstract] | an aggregated BSDF a voxel, cost independent of the scene | offline |

- **Without Nanite:** a GodotCon 2026 talk copies the far voxels on stock Godot, 200-800k trees in frame, no timings [V abstract]; Voxel Farm (2012) merged far trees into canopy volumes per area, blending into the terrain past 1 km [V]. No real-time paper 2020-2025 on voxel vegetation levels for rasterisers found; nothing on shadows or TAA for a standalone version.

## What it means here [inference]

- **A pixel's size:** about 0.85 mm a metre of distance at 1440p and a 70 degree view: 0.25 m at 300 m, 0.85 m at 1 km.
- **Per variant:** sparse 4³ bricks at 0.25, 0.5 and 1 m voxels, an occupancy mask a brick, 4-6 B a voxel (colour, a packed normal lobe): about 150-250 KB a variant.
- **Drawing, three ways:** a box a brick ray-marched in its fragment shader writing conservative depth (fits the prepass if the main pass repeats the depth; cost with covered pixels times steps); a cube a voxel by hardware raster (simplest, wastes 2x2 quads, cost with voxels); a compute rasteriser into a visibility buffer (Nanite's; a rewrite, not forward+).
- **A volume a cell:** each 128 m cell's far trees in one brick volume, 30-60 MB a km² at 1 m voxels, 4-8 MB at 2 m; re-baked when planting changes; cost with cells.
- **A shell on the terrain (Bruneton):** canopy height plus prefiltered colour and density at 1 m, about 8 MB a km²; cost with pixels only; the cheapest and the roadmap's `far-canopy` already.
- **Shadows:** the bricks or shell into the far cascades only; binary opacity gives clean depth. **TAA:** dithered coverage from the mips, a dithered fade from mesh to volume.
- **Risks:** holes and over-dense crowns (unsolved by Epic), flat noisy normals, shader-written depth losing early depth tests, baking tools; and the gain is unproven: measure what the far cards cost first.
- **Order if built:** the terrain shell (`far-canopy`), then volumes a cell, then bricks a tree; each testable alone.
