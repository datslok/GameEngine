# Leaves as geometry: research

Phase 21 stage 3: the trees' alpha-cut twig cards replaced by modelled, opaque leaves drawn as mesh clusters. Follows `VIRTUALGEOMETRY.md` ("Leaves as geometry") and `FARFOLIAGE.md` (far crowns). [V] primary source read, [S] secondhand, [I] inferred.

## Where we stand

- Only the trees use cut-out cards: 6 a leafy node plus 32 fillers a clump, 1 m twig cards of 12-18 leaves from a 2x2 atlas, drawn as 8-12-corner outlines (`appendClusterCards`, `forest_trees.cpp`); about 12K cards a big tree (400K for ~33 trees, `phase-18-2.md`). Palms, climbers and epiphytes already have opaque outlined leaves.
- After stage 2, 83-85% of the cluster triangles on screen are these cards (one level, swaying, so neither cut nor culled; `phase-21.md`).
- `ClusterLeaf` (`leaf_clusters.h`) already records each leaf's base, angle, length and source cell in a twig; `clusterOutline` already traces a convex outline from alpha; `LeafSet024` holds nine leaves with opacity maps.
- Wind bends about each draw's model origin by height (`windOwnSway`); no per-vertex wind data, no branch hierarchy. Swaying draws skip the cluster frustum, facing and occlusion tests (`meshClusterRigid`).
- Shadows come from the shadow stand-ins (cut-out) in the cascades; placed plants are not in the TLAS.

## Assemblies (Nanite, UE 5.6-5.7)

Source: [Nanite Assemblies](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-assemblies), [Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage) [V].

| Point | What |
|---|---|
| Shape | a base mesh (trunk, big limbs) plus up to 65k instances of part meshes (twigs); one level of instancing only |
| Culling | a part's transform computed on demand while culling descends the hierarchy; parts near are their own instances |
| Far | parts' root clusters merged into the assembly's coarsest levels at build time (an internal HLOD); far, the whole tree is one simplified cluster |
| Wind | each part bound to bones and moved rigidly, unskinned; example tree 850 bones, 2,160 parts; 100k bones about 0.1 ms; animation stops below 0.1 of the screen |
| Numbers | Witcher 4 demo trees 1-10M triangles, 500k instances; largest tree 3.5 GB to 29 MB on disk |
| Why | Karis 2021 [V]: a mesh's DAG ends at one 128-triangle root, so tiny instances stop scaling; 10M instance matrices 457 MB; "merged unique proxies must replace instances at extreme distances" |

The culling algorithm itself is unpublished (no talk slides found).

## Keeping the crowns from going bare

- **Failure** [V, [meshoptimizer #1093](https://github.com/zeux/meshoptimizer/pull/1093)]: similar leaves collapse at similar errors ("coordinated omission"): many clusters flip at once, pop, and the canopy erodes.
- **`clodConfig::simplify_dilate_borders`** (meshoptimizer 1.3, in our `external/`): moves open-border vertices out after each simplification to restore lost area, from each border triangle's area/perimeter, miter capped near 45 degrees; Nanite's Preserve Area equivalent; foliage only. Off by default.
- **Nanite Preserve Area** [V]: dilates open boundary edges; "all foliage meshes and nothing else"; [S] about 10% more storage, dark spikes reported far in 5.2.
- **Fallbacks:** `simplify_fallback_sloppy` may merge nearby disjoint leaves into blobs (error x2); `meshopt_SimplifyPrune` deletes isolated parts (whole leaves): keep it off.
- **Stochastic pruning** [V, Cook & Halstead, Pixar]: kept share u = z^(-log2 h), z = 1 where elements reach about a pixel, h the distance halving them; one random rank an element (stratified so neighbours spread), stored in reverse order so a level reads a prefix; survivors' area x 1/u (leaves widened only); contrast kept by c' = c_mean + a(c - c_mean), a^2 = min(u k1 z^2, kmax) / min(k1 z^2, kmax); fades over a band t = 0.1. Our card thinning is this already (`card_thinning`).
- **AMD mesh-shader grass** [V]: 32 to 2 blades a patch with distance, survivors widened by the ratio; same idea.

## Wind with valid bounds

- Nanite: bones over vertex offsets: tight bounds, fixed-function raster kept; vertex offsets need a worst-case pad that inflates bounds and overdraw [V].
- Nanite Foliage ([docs](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage)) [V]: offset wind "adds per vertex calculations and forces sub-optimal cluster bounds"; skinning through a bone hierarchy instead, bounds "calculated from skinning matrices"; bones grouped in simulation groups (bone chains, wind per group), global wind direction only; skinning off below 0.1 of the screen by default (`r.Skinning.DefaultAnimationMinScreenSize`), those instances drawn unskinned in bind pose; VSM "requires you to disable animation in the distance". Weights and joints unpublished.
- GPU Gems 3 ch. 6, Zioma ([chapter](https://developer.nvidia.com/gpugems/gpugems3/part-i-geometry/chapter-6-gpu-generated-procedural-wind-animations-trees)) [V]: a branch record each (origin as pivot, main axis, a tangent across its parent's, stiffness, parent); each a rigid segment turning at its joint; angles from phase-shifted cosines (a random phase a branch), stiffness scaling the amplitudes; quaternions concatenated from the branch to the root, 2-3 levels enough. Cracks: **two-bone blend**, a vertex weighted between its branch and the one above by distance from the branch's origin over stiffness. DX10 moved the transforms to a separate simulation pass (stream-out) feeding the skinning as instance data. 8800 GTX: 30 trees / 2,400 branches 1.34 ms, two-bone skinning alone 0.86 ms; 1,000 trees / 80,000 branches 37.6 ms. Far: trunk motion only (simulation level of detail).
- Pivot Painter 2 ([post](https://tianc377.github.io/posts/TreeWindAnimationWithUnrealPivotPainter)) [S]: each element's pivot, parent, x axis and extent in textures; turn axis `normalize(cross(XVector, WindDir))`; ancestors found by repeated parent lookup in the material, each level's offset about its own pivot added; 4 levels (trunk, branch, small branch, leaf); 30k elements.
- Crysis (GPU Gems 3 ch. 16) [V]: main bend by height renormalised to the radius (motion stays in a sphere), detail bend from four triangle waves; per-vertex stiffness and phase.
- SpeedTree, Horizon [S]: three levels (whole tree, branch, leaf), leaves fastest.
- Habel et al. 2009, Physically Guided Animation of Trees [V, abstract]: branch response to turbulence precomputed in frequency space; hierarchical vertex displacement in one vertex shader, thousands of branches.
- [I] A two-bone blended vertex lies between its two bones' images, so a sphere holding both bones' images of a cluster's sphere bounds it with no pad from the wind.
- [I] Rigid twigs on today's per-vertex bend (measured, Nov 22 2026, forest twigs 1 m): 8-14 cm from today's offset at 7 m/s, 25 cm at 10 m/s, mostly the branch bob's 4 m wave across the twig; the bend's own shear (upright lines tilt 2 lean h / H, level ones lean h / H) 1.5-6 cm.

## Cost of small triangles

- Karis 2021 [V]: hardware raster sets up at most 4 triangles a clock and shades 2x2 quads; mesh and primitive shaders "still bottlenecked"; Nanite's software raster 3x faster for clusters with edges under 32 px; aggregates (leaves, grass) Nanite's weak case from overdraw and holes. 25M triangles rasterised in the UE5 demo frame.
- No measured quad-waste numbers for mesh shaders found. [I] For us opaque leaves trade cut-out pixel cost (shadows 17 ms of cut-out pixels in phase 18) for more, smaller triangles; the main-pass gain is uncertain until stage 5.

## Shadows

- Virtual shadow maps [V]: deforming geometry invalidates its cached pages every frame; "Rigid" mode and a distance turning offsets off exist; Nanite Foliage "requires disabling animation in the distance". No numbers published.

## What it means here [I]

- **Memory, baked a variant:** 12K twigs x 15 leaves x about 8 triangles is about 1.4M triangles a big tree variant; disjoint leaves at about 7 vertices each, about 60-100 MB a variant with levels; 11 variants 0.7-1 GB. Fewer, larger leaves (the user's 30-40 cm) cut this 2-4x. Assemblies: a few MB.
- **Assemblies here** would need a new culling level (twig instances a tree, expanded on the GPU) and merged far proxies; cost grows with visible twigs. A baked mesh a variant uses today's pipeline unchanged; memory grows with variants, not trees.
- **Levels:** clusterlod with `simplify_dilate_borders` (automatic, risk of blobs and erosion) or our own leaf DAG by prune rank (the card-thinning maths moved into the hierarchy; watertightness not needed between leaves; tested as in 2b).
- **Wind (the user's picks, Nov 22 2026):** bones from the skeleton (chains cut about every 2 m, two-bone blend along a chain and at its joint), a twig a bone with its flutter as that bone's turn; transforms in a pass once a frame, parents first; clusters within one chain holding their run of bones, bounds from the bones with no pad; animation stopped below 0.1 of the screen, which stage 4 needs.
- **Far:** the crown impostor card stays until volumes (`FARFOLIAGE.md`).

## The plan (phase 21 stage 3)

| Stage | What | Tests and checks |
|---|---|---|
| 3a | Twig prototypes: a `LeafSet024` leaf traced from its opacity (3 rows across the midrib, 12 triangles, folded and curled, opaque, light through it); twigs placed as `ClusterLeaf` places them; half the leaves a level by a fixed rank, the survivors widened | area kept a level, a level a prefix of the one below, ranks fixed |
| 3b | Trees draw twigs, expanded on the GPU while culling (assemblies, over a CPU bake); the cards beyond the twigs' reach and for shadows until 3f | i: levels given by the caller, as 2b; ii: twigs laid out as the cards; iii: twig pass, normal blend, a leaf's tone from a hash |
| 3c-ii | Bone motion in `wind.glsl` and its C++ mirror: each bone's turn from the tree's spring, stiffness and phase, about the axis across its direction and the wind; parents composed first; the blended vertex; a cluster's bound from its bones | still air still, the foot fixed, joints continuous, bounds holding over random poses; a GPU test (not run) |
| 3c-iii | The bones pass, its pool and buffers; bark and twigs moved by their bones in every view (twig flutter as its bone's turn); the trees' per-vertex sway retired | the user checks and tunes the sway |
| 3c-iv | Clusters built within chains, culled by their bones' bounds (cones turned with them); nothing kept whole for sway on trees; animation stopped below 0.1 of the screen | F4 counts, `cluster-compare`, timings at the four spots |
| 3e | Far levels above the twigs: whole twigs pruned by rank and merged into each variant's coarse levels, the cut crossing from twigs to tree without cracks or pops | |
| 3f | The leaf cards and shadow stand-ins retired, leaves casting opaque shadows in the cascades until stage 4; the crown's impostor card kept far until volumes | measured at the four spots |

- Plants, grass and hanging things keep today's sway. Grass and floor cover stay as they are: stage 1 culls their patches by cluster, stage 4 decides their shadows (contact shadows instead, measured), stage 5 helps them most.
