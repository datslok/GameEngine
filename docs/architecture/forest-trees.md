# Forest: trees, lianas, climbers and epiphytes

Covers: `src/games/forest/forest_trees`, `src/games/forest/leaf_clusters`, `src/games/forest/lianas`, `src/games/forest/near_trees`, `src/games/forest/climbers`, `src/games/forest/epiphytes`

The grown trees and what grows on them. Research: `docs/research/TREES.md`, `docs/research/JUNGLE.md`, `docs/research/VINES.md`. The game: `forest.md`; tubes and leaf cards: `forest-rock.md`, Strands, tubes and cards.

## Species

Three, each a `ForestTreeSettings` (F5 Forest, Canopy trees, Emergent trees, Mid trees; start-up), a few variants grown, each placed many times turned and scaled.

| | Canopy (defaults) | Emergent (`emergentTreeSettings`) | Mid (`midTreeSettings`) |
|---|---|---|---|
| Floor a hectare asked (in the patch), variants | 30 (79), 4 | 2.8 (9), 3 | 480 (1,212), 4 |
| Height, fork | 38, 18 m | 55, 36 m | 15, 10 m |
| Crown radius | 16 m | 14 m | 5 m |
| Limbs, clumps (radius) | 4, 14 (5.5 m) | 5, 12 (5 m) | 3, 5 (2.2 m) |
| Attractors | 5000 | 5000 | 1500 |
| Twig radius | 3.5 cm | 4.5 cm (a trunk about 1.5 m at the foot) | 2 cm (about 25 cm) |
| Buttress, lean | 1.5, 2.5 m | 2.5, 1 m | 0.3, 1.5 m |
| `spacingShare` | 0.55 | 1 | 0.6 |

`spacingShare`: how close two trunks of a species stand, as a share of the larger crown's radius, so crowns interlock.

## Growing a variant (`growTreeMeshes`)

`growTreeMeshes(settings, seed, barkTileMetres, outlines)` → `TreeMeshes`, the same seed the same tree, in its own frame (foot at the origin).

1. **Shape** (`treeVariantShape` → `TreeVariantShape`): the crown (`treeCrown` → `CrownEnvelope`, an ellipsoid from `forkHeight` to `height`, `crownRadius` across; `insideCrown`, `crownOutward`, `crownAttractors`), the fork leaning 30-100% of `lean` its own way (`leastLeanShare`), the limbs one fewer to one more than `limbs`, two at least.
2. **Clumps** (`treeClumps` → `TreeClump`): `clumps` spheres of 75-125% of `clumpRadius` by dart throwing, centres 55-100% of the way out of the envelope shrunk by their radius, `clumpSpacing` (0.9) of their radii's sum apart; sorted by angle round the trunk and cut into equal runs, one a limb. `clumpAttractors` spreads `attractors` through them by volume, none between, so the crown keeps gaps and depth.
3. **Scaffold** (`growScaffold`, `appendTrunk`): the trunk from `treeSinkMetres` (1 m) underground to the fork as base + rise s + lean s², in steps of at most 0.5 m; then a limb towards each run's clumps' middle, kinked sideways by smooth noise (`limbWander` 0.45 of the pull, changing over `limbWanderMetres`, 3 m), until within a clump's radius.
4. **Colonisation** (`coloniseTree`, Runions et al. 2007): each round every live attractor finds its nearest node (a grid of influence-wide cells); within `kill` (1 m) it is used up, within `influence` (4 m) it pulls; each pulled node grows a child a step along its pulls' unit sum plus `TreeGrowth::gravity` (`treeGravity`, 0.2) times down, unless a node lies within a tenth of a step there. Stops when nothing pulls, nothing grew or after `maxRounds` (300). Skeleton: `TreeNode {position, parent}`, parents first. `growTree` (trunk to an envelope's bottom, then colonisation) remains for one-envelope trees.
5. **Radii** (`leonardoRadii`): Leonardo's rule r_parent^n = Σ r_child^n with `leonardoExponent` 2.5 (limbs stay thick against twigs), from `twigRadius` at every tip (`treeTips`), one pass backwards; the trunk then tapers to `trunkTaper` (half again) thicker at its foot than at the fork.
6. **Bark tubes** (`branchChains`, `tubeSides`): the trunk's chain follows the thickest child (lower index on a tie) to a tip; every other child starts a chain at its parent's node at its own radius, so branches start inside their limb. Sides about every 15 cm round, 3 to 16; the trunk `lobedTrunkSides` (64) with `buttressCount` (5) fins (`buttress` trunk radii at the ground, falling by e every `buttressHeightMetres`, 1.5 m, `buttressSharpness` cos^24). Bark: Poly Haven `jolcham_oak_bark_01` (`forestTreeBarkLayer`), whole 1.5 m tiles round each tube.
7. **Leaves:** cluster cards (below).
8. **Kept for growths:** `TreeMeshes::trunk` (a `ClimbHost`, for climbers) and `TreeMeshes::skeleton` (a `TreeSkeleton`: nodes, radii as built, chains, `trunkRadius`, for epiphytes).

## Leaf-cluster cards

A twig of 12-18 leaves on each card, so more leaf for fewer cards (`docs/research/TREES.md`, Part 2).

- `appendClusterCards`: `cardsPerNode` (6) cards at every leafy node (`leafyNodes`: at most `leafyRadius`, 6 cm, thick) at least `shellShare` (0.3) of its clump's radius out of the clump it is deepest in (`clumpOf`), each `clusterSize` (1 m) a side, its front a random turn from out of its clump, the twig reaching out and up; then `fillCards` (32) cards 1.5 times as big near each clump's middle, facing every way, so the crown is not seen through.
- Normals (`foliageNormal`): `cardNormalShare` 0.3 the card's facing, `clumpNormalShare` 0.5 out of its clump, `crownNormalShare` 0.2 out of the crown, the last two cone-flattened underneath (`coneFlattened`), never past square to the card's front: each clump gets a lit top and shaded underside.
- **Outlines** (`ClusterOutlines`, `clusterOutline`): each texture cell's outline, its square clipped by four pairs of parallel cuts round its texels over the cut-out threshold, `clusterOutlineMargin` (4 texels of 1024) out for the mips' blur; every card is drawn as its cell's outline (`appendOutlinedLeafCard`), so far fewer pixels run the cut-out test.
- **Texture** (`leaf_clusters`, `forestLeafClusterMaterial`): composed at start-up (`composeLeafClusters` → `LeafAtlas`), a 2 x 2 atlas (`clusterColumns`, `clusterRows`) of `clusterTexels` (1024) cells: a stem up each cell (`clusterStemPoint`, `clusterStemBend`) and its leaves (`clusterLeaves` → `ClusterLeaf`: alternately left and right, turned 35-70 degrees out, the last near straight up, 70-100% of the leaf share, toned 0.85-1.1, warmed or cooled up to 6%), each sampled from one of `LeafSet024`'s nine leaves (`forestTreeLeafFolder`; `standInLeafAtlas` without the scan), its normal turned with it (`turnTangentNormal`), laid by alpha. Uncovered texels hold the leaves' mean colour at alpha 0, so coverage-keeping mips bleed no dark edge. Colour (tinted `forestTreeLeafTint`), normal and roughness maps. The leaf share is `leafLength` (0.22 m) over `clusterSize`; species whose share and texels match share one texture.

## Levels, shadows and cards

Engine side: `level-of-detail.md`. Per variant (`plantTrees` in `forest_game`):

| Part | Seen | Its levels | Shadows |
|---|---|---|---|
| Leaves | the cards, casting none | thinned card by card to `levelCardShare` (0.5), survivors grown by share^-`levelCardGrowth` (0.5), the level the mesh's first cards (`ClusterCardRun`, `nodeCardStride`); then the shadow leaves; then nothing at the card's error | — |
| Bark | the tubes, casting none | simplified to `levelBarkShare` (0.3); then the shadow bark; then nothing | — |
| Shadow stand-ins | never (`shadowOnly`, one model of two parts) | | shadow leaves: `shadowCardsPerNode` 1, `shadowFillCards` 8, `shadowCardScale` 2 (about a sixth of the cards); shadow bark: `shadowBarkShare` (0.1) by meshoptimizer |
| Card | far away only | hidden below `impostorErrorMetres` (1.5 m, at least the stand-ins' errors) | none (the stand-ins keep casting) |

- The impostor is baked from the bark's first level, the leaves, and the variant's climbers and epiphytes (`impostorFrames` 8, `impostorTexels` 128); every part chooses by the tree's one box, so exactly one of tree and card is drawn.
- The cards' level errors are how much longer their sides grew; the bark's how far the simplifier moved its surface.
- Trees are still, not static: no collision, not in the sky visibility's level, and the shadow stand-ins are hidden from rays.
- Sway: every part a `ForestSwayer` of the trees' group at the variant's height (`forest.md`, Sway).

## Placing trees

- `treePlantingLayer(settings, seed)`: a species' layer of the planting (`forest-plants.md`, The forest's layers) → `TreePlacement {foot, scale, turnRadians, variant}`, planted over `forestPatch` (z `treeNorthmost` -210 to `treeSouthmost` -40, within `treeSpread` 130 m of the maps' axis: `insidePlantedRegion`) until 2b plants round the camera. Each cell's tree 85-115% size (`smallestTreeScale`, `treeScaleRange`), any turn, on the lowest drawn ground on a ring `footReachMetres` (2.5 m) a unit of scale round the trunk.
- Kept when its crown sphere (`placedCrownCentre`, `placedCrownRadius`, the record's reach) fits the maps, it keeps clear of the layers before, then not within `spacingShare` of a candidate of its species with a lower threshold (the earlier cell on a tie): local, so any box decides alone.
- Order: emergents (`emergentPlaceSeed`); canopy trees (`canopyPlaceSeed`) on the emergents' 12 m lattice above their share, no trunk inside an emergent's crown; the mid layer (`midPlaceSeed`, 4 m cells) under and between them, trunks `midTrunkClearance` (4 m) from theirs.
- `treeReach` (the largest crown sphere) names the canopy cap a species reads.
- **Drawn by the GPU, no entity a tree** (stages 2b2 to 2c2): each species' variants are a `PlantedKindLook` (`treeLooks`, `getTreeLooks`: bark, leaves, card, the shadow's stand-ins, then the climbers' and epiphytes' parts, in the tree's frame); each model but the card sways as a tree (`treeLookSwayers`, given by `applySway`, which gives the field its looks anew). They are the field's looks over `forestPatch` from the start: the placed trees draw in every view and cast the shadows (`planting.md`, Placed plants drawn). The patch's placements stay on the CPU for `forest-climber`, `forest-epiphyte` and the start (numbered the same every run); the lianas' crowns come from the near cells (below). Trees were never static, so the sky bake never saw them; rays meet none (ROADMAP stage 2c3).
- **Sight lines** (`forestSightLines` → `SightLine`; the user's requirement): from eye height (`sightEyeHeight` 1.7 m) on the path every `sightLineSpacing` (10 m), round the meadow and round the clearing, to a point 250 m up the Monolith's face and to the Ring's top. They are the planting maps' corridors; `blocksSight` (`segmentMeetsSphere`) is the tests' check that nothing planted meets one.
- **Start among the trees** (`forestTreesStart`): `treesStartDistance` (12 m) south of the tree with the most others within `treesStartNeighbourhood` (60 m), facing it.

## Lianas (`lianas`)

Woody vines from the canopy and emergent crowns (`LianaCrown`), built after the trees (`buildLianas`). `docs/research/VINES.md`, stage 1.

- **Near cells** (`near_trees`, ROADMAP d2a): the big trees planted by the CPU a square cell at a time (`plantBigTrees`: the emergents' and canopy's layers alone, the same trees as the GPU's), each named by its layer and dither cell (`nearTreeIdentity`). `makeNearTreeCell` plants the cell grown by `neighbourSpan`, keeps the trees inside the drawn region, marks the cell's own (`LianaCrown::carries`, by the foot) and hangs their lianas. `buildLianas` joins the cells over `forestPatch` (`nearCellsOver`, `cellMetres` 64, F5 Forest, Near trees); the lianas are the same whatever the cells' size. Meshes cannot be freed, so the cells round the player wait for the GPU-drawn lianas (d2b).

- **Which** (`chooseLianas` → `Liana`): each carrying crown draws from its own stream (the seed and its identity), so a cell's lianas need only its crowns and their neighbours, in `plantBigTrees`' order; each liana keeps its own `seed` (its ivy). A crown carries some with `treeShare` (0.8), `pathShare` (0.95) within `pathReach` (15 m) of the path; 1 to `mostPerTree` (8). A rope (`ropeShare` 0.35, or when no crown's centre is within `neighbourSpan`, 30 m) hangs to the ground 1-6 m off, sunk `lianaGroundSink` (0.2 m), `ropeSlack` (up to 5%) longer than straight; a loop hangs into a random neighbour's crown with a sag of `leastSag` to `mostSag` (0.1-0.4) of its span, L = span (1 + 8/3 share²). Holds in a crown's lower part, so its leaves hide the ends.
- **Thickness** (`lianaDiameter`): Barro Colorado's census (Schnitzer et al. 2012) by a random share (half 1-2 cm, 42% 2-5, 7% 5-10, 1% 10-20), times `thickness` (2), so the thin ones still cover a pixel.
- **Shape** (`catenaryStrand`): the catenary of its length between its holds, solved for a by halving in doubles, points evenly spaced by the arc length's inverse, at most `lianaStepMetres` (0.5 m) apart; the straight line without slack or with holds one above the other.
- **Meshes** (`lianaMeshes` → `LianaMeshes`): grouped by the 40 m square of their first hold, a job a chunk; bark tubes of 4 sides (6 from `lianaThickDiameter`, 5 cm) in the trees' bark; ivy cards `leafSpacing` (0.35 m) apart inside crowns and `sparseLeafSpacing` (1.2 m) along the rest. Still, drawn only, shadows in the nearest `lianaShadowCascades` (2).
- **Wind:** each liana `hangBetween` two anchors: a `WindAnchor` in its crown moving with that tree's sway, and the other crown's or a fixed ground anchor; the anchors live in the air entity's `WindAnchors`.
- `ForestLianaSettings` (F5 Forest, Lianas; start-up).

## Climbers (`climbers`)

Root climbers (philodendrons, monsteras) up the canopy and emergent trunks, grown into each variant in its own frame (`growClimbers` from `TreeMeshes::trunk`; none for the mid layer). `docs/research/VINES.md`, stage 2.

- **The host:** `ClimbHost` and its `tubeFrames`; `hostSurface` (`HostPoint` at a height and angle, at the lobed radius) and `hostClearance` follow the drawn surface exactly.
- **How many** (`climbersOnTree`): `treeShare` (0.8) of variants, 1 to `mostPerTree` (4) each, from the variant's seed.
- **A climber** (`growClimber`): from 0.1 m up between the buttresses, winding `twist` (0.3) turns above 2.5 m, up `leastHeightShare` to `mostHeightShare` (0.5-1) of the trunk; a stem `stemDiameter` (3 cm) tapering to 60%, 5 mm off the bark. Below `shingleHeight` (4 m) shingle leaves (`shingleLeafLength` 0.2 m every `shingleSpacing` 0.12 m) flat on the bark; above, big leaves (`bigLeafLength` 0.8 m every `bigLeafSpacing` 0.4 m) hanging from stalks, as outlined cards. Feeder roots every `rootSpacing` (1.5 m) from 2.5 m up, pushed out until clear of trunk and buttresses, to the ground or, for `midAirShare` (0.3), part way, swinging from their own object (`windAnchorOwn`).
- **Leaf** (`aroidLeafImages`, `forestAroidLeafMaterial`): drawn at start-up, glossy and dark, a paler midrib and curving veins, a normal map; opaque, double-sided, roughness 0.4, light through leaves. Stems and roots in the islands' brown bark.
- **Drawn** until the tree's card takes over (a level drawing nothing at its error, chosen by the tree's box), and baked into the card; shadows in every cascade (`climberShadowCascades` 0).
- **`forest-climber [n]`**: the camera 7 m from the n-th tree carrying climbers, on its first climber's side (`getClimbedTrees`, `getClimbedFacing`), looking at the trunk 4 m up.
- `ForestClimberSettings` (F5 Forest, Climbers; start-up).

## Epiphytes (`epiphytes`)

Rosettes and moss on the limbs at the crown's base, grown into each variant of every species from `TreeMeshes::skeleton` (`growEpiphytes` → `EpiphyteMeshes`). `docs/research/VINES.md`, stage 3.

- **Sites** (`epiphyteSites` → `EpiphyteSite`): crotches (nodes with two children or more) and each limb segment's top, on limbs at least `limbShare` (0.25) of the trunk's radius and `leastLimbRadius` (3 cm) thick, no steeper than `steepestLimbDegrees` (35). `chooseEpiphyteSites` takes crotches first, then limbs, each in a seeded order, `spacing` (1.2 m) apart.
- **How many** (`epiphytesOnTree`): a big tree `leastPerBigTree` to `mostPerBigTree` (20-60), a mid tree up to `mostPerSmallTree` (5), room allowing.
- **A rosette** (`rosetteShape` → `RosetteShape`, `appendRosette`): strap leaves round by the golden angle, youngest steepest, oldest drooping, each the palms' folded leaflet (`appendStrapLeaf`, `StrapFold`). Bird's-nest ferns (`fernShare` 0.5 of a big tree's): `fernFronds` (12) of `fernLength` (1 m, 50-150% a rosette), a funnel arching over. Bromeliads (all of a mid tree's): `bromeliadLeaves` (16) of `bromeliadLength` (0.4 m), stiffer, channelled.
- **Atlas** (`epiphyteLeafImages`, `forestEpiphyteLeafMaterial`): twice as wide as tall, the fern's frond on the left (`fernColour`), the palms' leaflet picture on the right (`bromeliadColour`); opaque, double-sided, roughness 0.45, light through leaves.
- **Moss** (`limbMoss`, `moss` on): over runs of two segments or more at least `mossLimbShare` (0.15) of the trunk's radius and no steeper than `mossSteepestDegrees` (50), a shell `mossArcDegrees` (140) round the limb's top, 1 cm off the bark, narrowing to nothing at the ends; the `Moss001` scan as a surface (`forestLimbMossLayer`, 0.5 m tiles).
- **Drawn** as the climbers are; shadows in every cascade (`epiphyteShadowCascades` 0).
- **`forest-epiphyte [n]`**: the camera 8 m out from the n-th tree carrying rosettes' first rosette (away from its trunk) and 3 m above it, looking at it (`getEpiphyteViews`).
- `ForestEpiphyteSettings` (F5 Forest, Epiphytes; start-up).
