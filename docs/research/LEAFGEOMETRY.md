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
- Pivot Painter 2 [S]: each element stores pivot, parent, axis, extent; rotates about its pivot and inherits its parent's; 4 levels, 30k elements in the sample.
- Crysis (GPU Gems 3 ch. 16) [V]: main bend by height renormalised to the radius (motion stays in a sphere), detail bend from four triangle waves; per-vertex stiffness and phase.
- SpeedTree, Horizon [S]: three levels (whole tree, branch, leaf), leaves fastest.

## Cost of small triangles

- Karis 2021 [V]: hardware raster sets up at most 4 triangles a clock and shades 2x2 quads; mesh and primitive shaders "still bottlenecked"; Nanite's software raster 3x faster for clusters with edges under 32 px; aggregates (leaves, grass) Nanite's weak case from overdraw and holes. 25M triangles rasterised in the UE5 demo frame.
- No measured quad-waste numbers for mesh shaders found. [I] For us opaque leaves trade cut-out pixel cost (shadows 17 ms of cut-out pixels in phase 18) for more, smaller triangles; the main-pass gain is uncertain until stage 5.

## Shadows

- Virtual shadow maps [V]: deforming geometry invalidates its cached pages every frame; "Rigid" mode and a distance turning offsets off exist; Nanite Foliage "requires disabling animation in the distance". No numbers published.

## What it means here [I]

- **Memory, baked a variant:** 12K twigs x 15 leaves x about 8 triangles is about 1.4M triangles a big tree variant; disjoint leaves at about 7 vertices each, about 60-100 MB a variant with levels; 11 variants 0.7-1 GB. Fewer, larger leaves (the user's 30-40 cm) cut this 2-4x. Assemblies: a few MB.
- **Assemblies here** would need a new culling level (twig instances a tree, expanded on the GPU) and merged far proxies; cost grows with visible twigs. A baked mesh a variant uses today's pipeline unchanged; memory grows with variants, not trees.
- **Levels:** clusterlod with `simplify_dilate_borders` (automatic, risk of blobs and erosion) or our own leaf DAG by prune rank (the card-thinning maths moved into the hierarchy; watertightness not needed between leaves; tested as in 2b).
- **Wind:** a part index a vertex into a table of pivots and axes (trunk, limb, twig), turned rigidly in the shader; cluster bounds padded by the largest turn times the distance from the pivot so clusters stay culled; turns stopped past a screen size, which stage 4 needs.
- **Far:** the crown impostor card stays until volumes (`FARFOLIAGE.md`).
