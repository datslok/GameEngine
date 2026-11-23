# Leaves as geometry: research

Phase 21 stage 3: the trees' alpha-cut twig cards replaced by modelled, opaque leaves drawn as mesh clusters. Follows `VIRTUALGEOMETRY.md` ("Leaves as geometry") and `FARFOLIAGE.md` (far crowns). [V] primary source read, [S] secondhand, [I] inferred.

## Where we stand

- Only the trees use cut-out cards: 6 a leafy node plus 32 fillers a clump, 1 m twig cards of 12-18 leaves from a 2x2 atlas, drawn as 8-12-corner outlines (`appendClusterCards`, `forest_trees.cpp`); about 12K cards a big tree (400K for ~33 trees, `phase-18-2.md`). Palms, climbers and epiphytes already have opaque outlined leaves.
- After stage 2, 83-85% of the cluster triangles on screen are these cards (one level, swaying, so neither cut nor culled; `phase-21-1.md`).
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
- **Bone motion (3c-ii, the user's picks 1A 2A 3A, Nov 22 2026):** a bone turns about its pivot by the rotation vector `s (d x W)(1 + a sin t) + s b (W - d (d.W)) cos t`: d its rest direction (3A: every turn independent, only composing in order), W the tree's lean (x, z) as a vector, `t = 2 pi f seconds + phase`. `|d x W|` is the wind's share across the bone (drag on a cylinder), so a bone along the wind barely turns, with no special case.
  - Stiffness (1A): `s = 2 L / H min((r_foot / r)^k, most)`, k 3 from the cantilever (angle ~ q L^2 / E I, q ~ r, I ~ r^4), most a clamp; an untapered trunk then bends as today's parabola (tip `lean H`, tangent `2 lean h / H`).
  - Frequency a chain (branch), not a bone: the cantilever's first mode `f = 1.875^2 / (2 pi) (r / 2) sqrt(E / rho) / L^2` (L, r the chain's length and base radius; equal-length bones would make thin ones slower). sqrt(E / rho) about 3000 m/s for green wood: a 30 m trunk 0.3 m thick 0.28 Hz (measured 0.3-0.5), a 4 m branch 5 cm thick 2.6 Hz; twigs far faster, so clamped (leaves' mass slows them). Phase hashed a chain, so a branch's bones swing together.
  - Ellipse (2A): the swing along the wind (share a) and across it (share b, about 0.3) a quarter turn apart, both scaling with the lean, so still air is still. Across is `W - d (d.W)`'s turn, moving the bone at right angles to both itself and the wind: a trunk sways sideways, a horizontal branch across the wind bobs up and down (today's branch bob).
  - Pose `T(x) = q x + t`, composed parents first: `T_b(x) = T_parent(p_b) + q_parent Q_b (x - p_b)`, so a child's pivot stays on its parent. Vertex `w T_b(v) + (1 - w) T_parent(v)`; a cluster's sphere posed by each bone of its run and their parents, merged.
- **Bones on the GPU (3c-iii, the user's picks 1A 2A 3A 4A, Nov 22 2026):** posed once a frame for each tree a swaying view draws (camera, swaying shadow tiles; trees are placed plants, one identity across views), deduplicated by a per-frame table, within the sway's reach (until 3c-iv's screen cut), in a fixed pool (F5; a tree that does not fit stands still, F4 shows the need). Cost grows with drawn swaying trees' bones (Nanite: 100k bones about 0.1 ms).
  - Last frame's poses (2A) recomputed from last frame's springs and time, as the sway does today: no slot history.
  - Twigs (3A) rigid by their bone's blended pose at the foot, plus a flutter turn about it: work a twig, not a leaf vertex.
  - Every tree mesh bound (4A): bark's old levels and the shadows' leaf cards too, so a tree and its shadow move alike and the per-vertex sway can go.
- **The bones pass (3c-iii-b, the user's picks A A A, Nov 22 2026):** wind and bones moved before the cluster passes (3c-iv's bounds need the poses); a tree claimed once by a word a held plant (frame stamp and tree, no clearing, one read a vertex); a workgroup a tree posing a chain a thread, chain level by level between barriers (each bone once, 3-5 barriers a tree).
- **Reading the poses (3c-iii-c, the user's picks Nov 23 2026: split in four, still when unposed, flutter reused):**
  - A vertex finds its pose by its plant: the plant table's word pair, a tree only when its stamp is this frame's (`boneClaimedTree`; the frame's stamp in the wind record, 0 on a frame the pass does not run), the tree's first pose, its bone's pose (`bonePoseSlot`, last frame's after it) and its parent's (named in the pose's fourth word, so draws need no bone table). Scene bindings 50-52: trees, plant table, pool.
  - Unposed is still: a capture, a tile drawn still, a tree past the reach, unclaimed or without room keeps its rest shape (no fallback to the per-vertex sway: one path).
  - Bones live in the plant's frame: a part bound to them sits at its plant's origin (the placed table refuses another), so a vertex is posed in its mesh's frame, then placed.
  - i (`tree-bones-3c-iii-c-i`): every bound pool vertex (bark and its levels, cards, climbers, epiphytes) in the camera's passes (vertex and mesh shaders) and swaying shadow tiles; normals and tangents turned; motion vectors from last frame's poses; cards keep today's flutter (`windTreeFlutterAmount`, shared with the sway); a climber's own and tree anchors move as its bone. Twigs and lianas keep the old sway until ii and iii.
  - ii: twigs rigid by their binding's blended pose at their foot (binding 49), plus today's flutter. iii: lianas' tree anchors by their bone. iv: what is left of the trees' per-vertex sway retired (`windAnchorOffset`, the liana record's tree height, scale and bend); the branch bob stays, `windSwayOffset`'s for the plants without bones.
  - iii (the user's pick A, Nov 23 2026): the CPU's lianas know their trees' feet bit for bit (the same planting) but not their plant numbers (the GPU's slots, packed by prefix sum), so a pass finds them:
    - Records: `LianaShape` 144 B (128 again from iv), its anchors' bindings (the nearest bark to the hold in the tree's mesh frame, `swayNearestBinding`) and trees' field layers; the binding's packing moves to `scene/sway_bones`.
    - Lookup (a compute pass before the claim, a thread a liana anchor held): the foot's cell in its layer's set (by `plantingCellBox`'s edges), its slot from a table a set (side² places by column and row mod side, which the held cells never share), the slot's plants of that layer searched for the same foot (within 1 cm); the plant (or none) a word a liana anchor, scene binding 53.
    - Drawn: the anchor posed as a bound vertex at the hold (`boneDrawnBy`, its plant's frame from the plant record); still when its tree is not posed; the swing below as today.
    - Not claimed by the lianas: a liana's crown is nearly always in a swaying view when the liana is; a crown culled while its liana shows leaves that end still (3c-iv's bounds may revisit).
    - Cost: the lookup grows with held lianas (most stop at the reach test) and a cell's trees for those in reach, about 0.01 ms; a liana vertex reads two words, two plant records and four poses.
  - Cost a bound vertex: a binding word, the plant's pair, a tree record and two poses (four in the main pass), against today's one or two spring samples: about even; grows with drawn bound vertices.
  - [S] Unreal Dynamic Wind ([Nanite Foliage](https://dev.epicgames.com/documentation/en-us/unreal-engine/nanite-foliage), [Dynamic Wind](https://dev.epicgames.com/documentation/unreal-engine/dynamic-wind-for-nanite-foliage-in-unreal-engine)): bones simulated on the GPU, each its own spring (mass, drag area, centres baked; "no full constraint solver"), `DynamicWind.Mode 0` stateless; plants under the global wind share "a small set of simulations, one per wind direction", a plant reached by a force, push capsule or volume promoted to its own (budget 256 a mesh, back after 3 s); animation off below 0.1 of the screen; 100k bones about 0.1 ms; ordering and layout not documented.
  - [S] GPU hierarchies ([arXiv 2505.06703](https://arxiv.org/html/2505.06703v1)): a thread a joint walking to its root wastes work in deep skeletons; barriers near 40% of a shallow skeleton's cost, so few levels between barriers.
  - [I] Ours is per tree and stateless (Mode 0's kind) but spatial: each tree's lean from the field at its foot. Later and optional: per-bone springs for a budget of near trees with push forces (phase 15's hero), shared poses a (variant, wind cell) for far trees if BONES shows in timings.
- **Bounds from the poses (3c-iv, researched Nov 23 2026):**
  - [V] Nanite Foliage: bounds "calculated from skinning matrices"; the method is unpublished. [S] Unreal 5.5's skinned Nanite is said to bound clusters per bone; a design note elsewhere ([GodotNext #159](https://github.com/Legoless/GodotNext/issues/159)) stores each cluster's bones with a box a bone, the posed bound their union.
  - [V] Unterguggenberger et al. 2021, [Conservative Meshlet Bounds for Robust Culling of Skinned Meshes](https://www.cg.tuwien.ac.at/research/publications/2021/unterguggenberger-2021-msh/): offline, a box a meshlet relative to its most influential bone, grown along the hierarchy over every pose an animation may take; normals bounded by a cone; sampling poses cannot guarantee bounds. Suits fixed clips, not a stateless wind whose poses exist each frame.
  - [S] The common way ([gamedev.net](https://gamedev.net/forums/topic/649828-how-to-compute-the-bounding-volume-for-an-animated-skinned-mesh/5108620/)): each bone's rest bound posed by its transform, merged; work a bone, not a vertex.
  - [I] Ours: a bound vertex lies between its bone's and parent's images, so a cluster's rest sphere posed by each of its bones and their parents and merged (`windPosedSphere`, `windSphereUnion`) holds it with no pad. A tree's whole move this frame bounds any part: each bone's pivot and tip moves plus `2 sin(angle / 2)` times the farthest bound point from it.
  - [I] Our trees (3c-i's counts): emergent 389 chains for 18.0K bark vertices, canopy 474 for 17.5K, mid 34 for 2.9K: 37-85 vertices a chain, about one cluster's 64. Cutting clusters within chains would split most clusters at chain ends and stop the levels merging below one cluster a chain (389 for an emergent tree), so the cut could never coarsen a tree past that.
  - [I] The cut keeps its rest spheres: a group's error is tested the same way by every cluster, so the cut stays closed; the sway moves a sphere tens of centimetres, a negligible change in pixels.
  - [I] The animation cut by the tree's size on the camera's screen (one choice for every view, as the claim is): a 30 m tree at 0.1 of a 90-degree screen is about 150 m away, a 10 m one about 50 m; cost grows with large swaying trees on screen, not trees within a reach.
  - **The user's picks (Nov 23 2026): B, A** (over 3c-i's clusters within chains, for the levels' sake):
    - Clusters built as today, each with up to 8 of its vertices' bones (`GpuMeshCluster::bones`, 128 B); more, its rest sphere grown by its tree's move.
    - Measured (seeds 7 and 21): full-detail bark 78-84% of big trees' clusters within 4 bones, the rest 5-8 (where about four chains meet); cards 80-89%, 3 of 611 past 8; the first coarser level 48-61% within 4, 2% past 8; the second a third past 8. So 8 (the user's pick for scale: a fixed cost a cluster) holds full detail exactly; a list of any length only if bones shrink.
    - A tree's move a frame (the bones pass): its bones' largest pivot or tip move E and largest `2 |turn.xyz|` A; any point within its skeleton's reach R (`SwaySkeleton::reach`) of its bones, blended or rigid between two, moves at most E + A R.
    - Animation stopped below 0.1 of the screen (the tree's sphere's diameter over the screen's height), faded below it, decided by the camera for every view; F5.
    - Split: iv-a the bones and the move; iv-b culled by them (frustum, occlusion); iv-c the screen cut.
  - [I] No facing test on posed clusters (found building iv-b): a blend shears triangles near a joint, so a normal turns by more than its bones do and no widened cone is safe; the flutter moves leaves too. Twigs move rigidly, but flutter as well.
- **Far:** the crown impostor card stays until volumes (`FARFOLIAGE.md`).

## The plan (phase 21 stage 3)

| Stage | What | Tests and checks |
|---|---|---|
| 3a | Twig prototypes: a `LeafSet024` leaf traced from its opacity (3 rows across the midrib, 12 triangles, folded and curled, opaque, light through it); twigs placed as `ClusterLeaf` places them; half the leaves a level by a fixed rank, the survivors widened | area kept a level, a level a prefix of the one below, ranks fixed |
| 3b | Trees draw twigs, expanded on the GPU while culling (assemblies, over a CPU bake); the cards beyond the twigs' reach and for shadows until 3f | i: levels given by the caller, as 2b; ii: twigs laid out as the cards; iii: twig pass, normal blend, a leaf's tone from a hash |
| 3c-ii | Bone motion in `wind_bones.glsl` and its C++ mirror: each bone's turn from the tree's spring, stiffness and phase, about the axis across its direction and the wind; parents composed first; the blended vertex; a cluster's bound from its bones | still air still, the foot fixed, joints continuous, bounds holding over random poses; a GPU test (not run) |
| 3c-iii | The bones pass, its pool and buffers; bark and twigs moved by their bones in every view (twig flutter as its bone's turn); the trees' per-vertex sway retired | the user checks and tunes the sway |
| 3c-iv | Clusters culled by their bones' bounds (up to 8 a cluster, else the tree's move; no facing test while posed); nothing kept whole for sway on trees; animation stopped below 0.1 of the screen | F4 counts, `cluster-compare`, timings at the four spots |
| 3e | Far levels above the twigs: whole twigs pruned by rank and merged into each variant's coarse levels, the cut crossing from twigs to tree without cracks or pops | |
| 3f | The leaf cards and shadow stand-ins retired, leaves casting opaque shadows in the cascades until stage 4; the crown's impostor card kept far until volumes | measured at the four spots |

- Plants, grass and hanging things keep today's sway. Grass and floor cover stay as they are: stage 1 culls their patches by cluster, stage 4 decides their shadows (contact shadows instead, measured), stage 5 helps them most.
