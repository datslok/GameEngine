# Forest: twigs of modelled leaves

Covers: `src/games/forest/twigs`, `src/scene/leaf_pruning`

Phase 21 stage 3 (`ROADMAP.md`; research in `docs/research/LEAFGEOMETRY.md`): twigs of opaque, modelled leaves, which the trees will place as instances in place of their cut-out cards (`forest-trees.md`, Leaf-cluster cards). So far (3a, 3b-i, 3b-ii) the prototypes, their levels as given cluster levels, a stand to look at them, each tree variant's twig list and its twig set for the renderer; no tree draws them yet.

## A modelled leaf (`leaf_clusters`)

- **Traced** (`traceLeafShape` → `LeafShape`): a cell of the single-leaf atlas (`forestTreeLeafAtlas`: `LeafSet024`, or `standInLeafAtlas`) as rows across the midrib (x = 0.5 of the cell): each texel row's covered extent left and right (alpha at least `forestLeafCutoff`), the base and tip at the lowest and highest covered rows, everything pulled `insetTexels` inside the alpha's edge. The rows' heights are chosen from 24 steps between base and tip (`leafRowSteps`) by trying every choice, keeping the best overlap over union, row by row.
- **Fit:** the corners lie on the leaf's edge, so a convex leaf is at best a polygon inside it: on an ellipse three rows reach a regular octagon in a stretched circle, 2 sqrt 2 / pi (90%) of the area.
- **Built** (`appendModelledLeaf`, `ModelledLeafPlace`): the base, each row's left edge, midrib and right edge, the tip: 3 rows + 2 vertices, 4 rows triangles (12 for 3). Each half raised by its distance from the midrib times tan(fold); the tip dropped by curl t^2 (t base to tip), both times the cell's side in metres. UVs are the shape's own cell units mapped into the atlas cell (`atlasCell`); normals the triangles' area-weighted mean (the fold a soft ridge); tangent across the leaf, w = 1.
- **Material** (`forestTwigLeafMaterial`): the single-leaf atlas, tinted `forestTreeLeafTint`, double-sided, opaque (nothing alpha tested in any pass), with light through it (`withLeafLight`).

## A twig (`twigs`)

- **Leaves** (`buildTwig`): laid out as a card's picture lays them (`clusterLeaves`, the same seed gives the same twig), a cell `clusterSize` (1 m) a side, the leaf share `leafLength / clusterSize`; each leaf pitched and rolled out of the twig's plane by up to `tiltDegrees` at random (`leafFrame`). Frame: the stem's foot near the origin, the stem up +y, the leaves facing +z.
- **Stem** (`appendStem`): a three-sided tube up `clusterStemPoint`, `stemRadius` at the foot to a third at the tip, 5 rings; the finest level only, in the trees' bark.
- **Levels** (`TwigLevel`): each keeps `prunedKept` of the leaves, the prefix of `prunedOrder` with a seeded offset, and draws them with `twigLevelRows` rows (`leafRows`, then at most 2, then 1). The kept leaves grow about their bases by sqrt(the twig's full leaf area over the kept leaves' area at the level's rows), so every level keeps the finest's area exactly (Cook and Halstead). Error: the most any grown leaf strays past its size, length x (growth - 1), never less than the finer level's; 0 for the finest.
- **Prototypes** (`buildTwigPrototypes`): `prototypes` (8) twigs, each from its own seed (`forestShapeSeed` of `twigSeed`), built in `buildTwigs` after the trees from the canopy's settings.
- **For the cluster cut** (`twigLeafMesh` → `TwigLeafMesh`): every level's vertices in one mesh, the finest's triangles its indices, the coarser levels given with their errors (`buildGivenClusterLevels`, `mesh-clusters.md`), a chain of a group a level.
- **On the trees** (`clusterTwigs` → `PlacedTwig`, `TreeMeshes::twigs`, `getTreeTwigs`; 3b-ii): a twig where each leaf card is, one for one and in the cards' order (one walk for both, `forEachClusterCard`: every card, as the kept cards and the rest together make): the card's stem its foot, its frame the twig's (x across, y to the tip, z its front), its side over `clusterSize` its scale (fillers 1.5), the card's random number its pick (`twigPrototypeOf`), and its lean, the clump's and crown's share of the cards' normal blend (`foliageLean`), so `bentFoliageNormal` of its facing is the card's normal exactly.
- **For the renderer** (`treeTwigSet`, `getTreeTwigSets`; 3b-iii-a): in `buildTwigs`, each prototype as one mesh with its levels given (`twigLeafMesh`, `setGivenClusterLevels`), and each tree look's twigs over them in bricks (`makeTwigSet`, `mesh-clusters.md`), a twig's pick choosing its prototype.
- **A tree look's twig part** (`buildTwigs`, 3b-iii-b2a): one more model, last: its part's mesh a triangle across the twigs' box (their bounds) with the leaves' last level (nothing past the crown's card) and their level options, `twigs` its set, casting nothing, swaying as the leaves (`treeLookSwayers`); the leaves model (`treeLeavesModel`) its fallback.

## Pruning order (`scene/leaf_pruning`)

- `prunedOrder`: parts numbered from an offset, sorted by their numbers' bits reversed; level k's prefix is exactly the parts numbered a multiple of 2^k, so a level is a subset of the finer one's, evenly spread, never two neighbours gone at the first halving.
- `prunedKept` (count / 2^level rounded up, at least 1) and `prunedLevels` (until one is left).

## Settings and command

- F5 Forest, Twigs (`ForestTwigSettings`, read at start-up): prototypes, leaf rows (3: the user's pick over 2 or 4), fold (15 degrees), curl (0.08), tilt (15 degrees), inset (1 texel), stem radius (6 mm).
- `forest-twigs [off]` (`showTwigStand`): every prototype's every level on a wall in front of the camera, a row a prototype (the first on top), its levels left to right, facing the camera; off hides them. An entity a level (`getTwigStand`), still and hidden until shown.
