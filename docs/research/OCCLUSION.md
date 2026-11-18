# Occlusion culling

Research for phase 18's occlusion culling item: not drawing what hides behind other things. What the sources say, with their numbers; our own inferences are under "What this means for the engine". The plan stays in `ROADMAP.md`. Marked **[mem]**: from memory, not checked against a source.

## The two families

| Family | How | Who |
|---|---|---|
| GPU depth pyramid (Hi-Z) | Depth reduced into mips, each texel the farthest depth below it; each object's screen rectangle tested against one mip | AC Unity, RedLynx, Nanite, Splinter Cell: Conviction, niagara |
| CPU software raster | Chosen occluders rasterised on the CPU into a small depth buffer, objects tested there | Killzone 3 (SPUs, 640×360 → 40×23), Battlefield 3, CryEngine 3's coverage buffer, Intel's Masked SOC |
| Precomputed (Umbra) | Level voxelised into cells and portals offline | Killzone 4 **[mem: Umbra by name]**; Horizon dropped it ("Seriously, none": its bake times hurt the workflow) |

## Two-phase culling (the modern standard)

1. **Early:** draw what was visible last frame.
2. Build the pyramid from that depth.
3. **Late:** test everything else against it; draw the newly visible; remember who was visible for next frame.

- **Nanite** (Karis, SIGGRAPH 2021): "almost perfect… conservative, only falls apart under extreme visibility changes". Rejects reprojecting last frame's depth: "needs hole filling", "not conservative". [slides](https://advances.realtimerendering.com/s2021/Karis_Nanite_SIGGRAPH_Advances_2021_final.pdf)
- **RedLynx** (Aaltonen, SIGGRAPH 2015): 250k moving objects, Xbox One 1080p: culling + level of detail 0.28 + 0.26 ms, pyramid 0.06 ms, 2.3 ms in all; 0.2 ms of CPU. [slides](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)
- **niagara** (Kapoulkine, Vulkan): a visibility flag per draw, read by the early pass, written by the late; pyramid built with a `VK_SAMPLER_REDUCTION_MODE_MIN` sampler (one sample = the min of 2×2). [source](https://github.com/zeux/niagara)
- First published by Nießner & Loop 2012 (patch-based occlusion culling for tessellation).
- Worst case, a sharp turn or teleport: most objects fall to the late phase. Correct, but no saving that frame.

## One-phase alternatives and their artefacts

- **AC Unity:** a prepass of the 300 best occluders (~0.6 ms PS4), downsampled to 512×256, merged with last frame's reprojected depth. 20-40% of triangles culled; 30-80% of shadow triangles.
- **CryEngine 3:** last frame's depth reprojected, tested on the CPU; holes filled as far. Small artefacts from the frame of delay. [summary](https://www.gamedev.net/tutorials/_/technical/graphics-programming-and-theory/coverage-buffer-as-main-occlusion-culling-technique-r4103/)
- **Splinter Cell: Conviction** (Hill & Collin, GPU Pro 2): 512×256 pyramid from occluders, ~22k objects, Xbox 360 0.52 ms in all. [notes](https://blog.selfshadow.com/publications/practical-visibility/)
- **Masked SOC** (Hasselgren et al., HPG 2016): ~4 ms on one thread, 98% of a full-resolution buffer's culling. [paper](https://diglib.eg.org/handle/10.2312/hpg20161189)
- Hiding the delay: inflated bounds (vkguide says Unreal's queries lag 3-4 frames), or current-frame occluders merged in.

## The test

- **Rectangle:** the bounding sphere projected (Mara & McGuire 2013, niagara's `projectSphere`, ~60 FLOPs, needs only P00 and P11). [Kapoulkine 2023](https://zeux.io/2023/01/12/approximate-projected-bounds/)
- **Near plane:** a sphere crossing it counts as visible.
- **Mip:** `ceil(log2(max(w, h) in pixels))`; one level finer when the rectangle touches at most 2×2 texels there; one min-reduced sample covers it (niagara). Nanite: rectangle ≤ 4×4 texels.
- **Reversed-Z:** the farthest depth is the **smallest**, so the pyramid takes the **min** (standard-Z engines take the max). Visible if the sphere's nearest depth > the pyramid's. Sky is 0, so nothing behind sky is ever culled.
- **Sizes not a power of two:** level 0 = the previous power of two of the depth size; the min sampler's footprint keeps it conservative; each level then halves exactly ([niagara discussion](https://github.com/zeux/niagara/discussions/15)). Bevy's odd-size pyramid is "slightly incorrect".
- **Granularity:** per instance for props; clusters of 64-256 triangles for big meshes (RedLynx 64 vertices, Frostbite 256 triangles).

## Output

- `vkCmdDrawIndexedIndirect(Count)` (core in Vulkan 1.2). Compaction by subgroup prefix sums (Wihlidal) or one atomic per subgroup (Decima); a plain atomic per thread makes the order differ frame to frame.

## Shadows

- **Receiver masks** (Bittner et al., I3D 2011): a caster whose shadow falls on nothing the camera sees is dropped; 3-10× on city scenes, 1.5-2× in a game scene. [paper](https://www.cg.tuwien.ac.at/research/publications/2011/bittner-2011-scc)
- AC Unity did it per cascade from the camera's depth in light space (~0.1 ms): 30-80% of shadow triangles culled; fog spoilt the mask's far end.

## Foliage

- No shipped game gives Hi-Z culling figures for forests (none found).
- Leaf cards write depth only where the alpha test passes; a farthest-depth pyramid keeps every hole, so a canopy with gaps occludes almost nothing at the coarse mips big objects are tested on ([forum](https://gamedev.net/forums/topic/537738-occlusion-culling-and-terrains/4474112/): "vegetation makes very poor occluders").
- The occluders in a forest: terrain, trunks, rocks, buildings. Decima keeps vegetation out of its static occlusion queries (generated at run time).
- Occluder stand-ins (simple solid shapes drawn only into the culling depth) are the standard fix for leaky occluders **[mem]**; for crowns they would drop objects behind a crown that is partly see-through.

## What this means for the engine

- Today: frustum, level of detail and batching on the CPU; each batch one `vkCmdDrawIndexed`; a depth prepass, then the main pass at `EQUAL`, so hidden pixels are never shaded already. **The saving is vertex and raster work** (prepass, main, velocity), not shading. In the forest that is wind-swayed leaf cards: likely real, size unknown until measured.
- Our depth pyramid (`depth_pyramid.comp`, for reflections) takes the nearest depth; culling needs the farthest. One shader with the reduction chosen, not a copy.
- Still draws come first in a fixed order (`still-objects`), so a visibility flag per still draw carries over between frames; moving draws can be tested each frame.
- The CPU keeps choosing batches; the GPU only writes how many instances of each survive (compacted draw list, indirect draws). CPU cost unchanged; GPU-driven batching would come with the persistent scene's last stage.
- Order of instances within a batch may differ frame to frame with atomics; harmless for the picture (prepass + `EQUAL`), a subgroup prefix sum keeps it fixed if golden images need it.
- Best views: from the rim behind pillars, behind the Monolith, trunks and ridges in the jungle. Weakest: inside the canopy looking through gaps.

## Measured in the forest (`occlusion-measure`, laptop, DLSS at full resolution)

The camera's draws tested against the farthest-depth pyramid of the same frame's prepass (an upper bound for two-phase culling; per-tree spheres, so slightly low).

| View | Objects hidden | Triangles hidden | PREPASS / MAIN / MOTION (ms) | GPU (ms) | CPU CAMERA (ms) |
|---|---|---|---|---|---|
| Up into the canopy | 4% | 0.6% | 2.3 / 10.9 / 1.8 | 29.7 | |
| Jungle, level at eye height | 50% (1,198 of 2,384) | 13% (718K of 5.5M) | 1.4 / 10.1 / 1.1 | 26.1 | 2.4 |
| Rim | 93% (10,783 of 11,544) | 15% (447K of 3.0M) | 0.9 / 8.1 / 0.4 | 18.0 | 9.4 |
| Ring | 22% | 2% | 2.2 / 8.0 / 1.7 | 21.1 | 9.0 |
| Wall, up the Monolith | 47% | 19% | 0.6 / 2.7 / 0.3 | 11.9 | 1.6 |

- What hides is many small draws (grass patches, floor cover, small plants behind ridges and trunks), not the leaf-heavy crowns, as the foliage sources predicted.
- GPU culling saves only vertex work (the prepass already keeps hidden pixels unshaded): about 0.3-0.5 ms a frame (2%), against two-phase culling's extra prepass, indirect draws and visibility tracking. Not built (the user, Nov 18 2026).
- The CPU's CAMERA stage (levels, batches, packing per draw) took 9 ms where 30-35K draws were in view, and 93% of them were hidden at the rim: culling on the CPU before that work is where occlusion pays here (ROADMAP).
