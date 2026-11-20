# Forest: understorey, palms, grass and floor cover

Covers: `src/games/forest/valley_maps`, `src/games/forest/understorey`, `src/games/forest/palms`, `src/games/forest/grass`, `src/games/forest/floor_cover`, `src/games/forest/ground_cells`

What grows under and between the trees. Research: `docs/research/JUNGLE.md` (the understorey, palms), `docs/research/GROUNDCOVER.md` (grass, the floor). The game: `forest.md`; level-of-detail and impostor machinery: `level-of-detail.md`.

## Planting maps (`valley_maps`)

Every layer (trees, palms, understorey) is planted from maps, not darts (Horizon Zero Dawn's way; research and the user's choices: `docs/research/VALLEY.md`). Built once at start-up (`buildPlantingMaps`, on the jobs) over the floor, walls and rim; a place's plants are the same whatever order or region asks, so stage 2 can plant any cell alone.

| Map (`ValleyMaps`) | Texel | Holds |
|---|---|---|
| `ecotopes` | 2 m, on a grid across the winding axis | four bytes summing to 255: floor, wall, rim, bare (`landEcotopes`) |
| `clearance` | same | decimetres to the nearest obstacle's edge, at most `farthestClearance` (60 m): a distance field, one for every reach |
| `caps` | 2 m, square to the world, over the lines' footprint | a `CanopyCap` a species' reach: the lowest any sight line passes within reach (plus half a texel's diagonal) of the texel's middle, decimetres |

- **Extent** (`ForestPlantingSettings`, F5 Forest, Planting): within 450 m of the axis (floor 200, wall 150, 100 of rim), z -5000 to 800. 450 x 2900 texels, caps 94 x 633: about 8.4 MB, built in about 0.1 s (release, laptop).
- **Ecotopes** (`landEcotopes`): across the axis floor, then wall, then rim, each blending into the next over `ecotopeBlendMetres` (30) round the land's own edges; bare by the clearing and the meadow (their radii plus `clearingEdgeMetres` 10), by slope (`bareSlopeFrom` 1.2 to `bareSlopeTo` 2) and outside the extent. Read blended (`valleyEcotopes`).
- **Obstacles** (`forestObstacles` → `ValleyObstacle` discs): pillars at `pillarBulgeShare` (1.3) radii, the Ring's feet (`ringFootShare` 1.5 tube radii), each tower's widest section's half diagonal. `valleyClearance` reads the texel less `texelReach` (half a texel's diagonal, more where the axis winds): a lower bound.
- **Caps** (corridors from `forestSightLines`): exact per texel (where a line enters and leaves the disc; straight, so its lowest point there is at one end), less 5 cm. `valleyCap(reach)` reads the smallest cap at least that reach; none throws.
- **Gaps** (`valleyGap`): `forestFractalNoise` over `gapMetres` (70), a gap from `gapFrom` (0.3) to `gapTo` (0.5): about 13% of the patch.
- **A layer** (`PlantingLayer`, in each species' settings, F5 ... Planting): plants a hectare asked on the floor, wall and rim; `footprintMetres` (its dither cells, one plant at most), `jitterShare`, `gapShare` (density in a full gap), `pathClearance`, `nearPathMetres`, `farPathMetres`, `farShare`.
- **Share** (`layerShare`): the ecotopes' densities weighted, times a cell's area, times 1 + (`gapShare` - 1) gap, times `pathDensityShare`; at most 1.
- **Dither** (`ditherThreshold`): Roberts' R2 over the lattice's cells, fract(column a1 + row a2) in 32-bit whole numbers, the same on every machine. A cell is planted when its threshold is under the layer's share at its middle (`ditheredCells` → `DitheredCell`, rows from the north); any band of thresholds is spread evenly, so neighbours never both fall under a small share.
- **Shared lattice:** a layer with the same footprint planted first (`under`: the emergents under the canopy) takes the thresholds below its share, so the two never take one cell.
- **Fits** (`fitsTheMaps`): a plant's sphere (reach, top) clear of every obstacle and `corridorClearance` (2 m) under every line. The test (`testWhatFitsBlocksNoSight`): nothing that fits meets a sight line or an obstacle.
- **Asked, not stood:** crowding (trees' `spacingShare`), trunks, the path and the corridors take their share, so a layer's per-hectare figures are above what stands (the patch: 9 emergents, 79 canopy, 1,212 mid trees, 8,640 understorey plants, 721 palms).
- **Cost:** the maps grow with the valley's area, once; a plant costs a few map reads and a hash, whatever else exists.

## Plant variants (`forest_game`)

`makePlantVariant` turns one grown or scanned plant into what is drawn (`PlantVariant`); `addPlant` places it. Shared by the understorey and the palms.

| Part | Drawn | Levels |
|---|---|---|
| Seen mesh | near, casting no shadow | its coarser level, then the shadow's stand-in (or, when thinned card by card, its own first vertices), then nothing at the card's error |
| Shadow's stand-in | `shadowOnly`, in the nearest `shadowCascades` (0: all; the nearest two ended them about 20 m away, popping in) | |
| Card | far only (`hiddenBelowError`), casting none | baked by `bakeImpostor` from the seen mesh with its texture, tint and cutoff, letting light through as the plant does |

All choose by the seen mesh's box, so exactly one of plant and card is drawn. Seen mesh and stand-in sway as plants at the variant's height; the card does not. With `fades` (the understorey's, not the palms', which thin card by card) and a cut-out material, the seen mesh and the card cross-fade by a dither over the camera's fade band instead of swapping (`fadesLevels`; level-of-detail.md, Level fades).

## Understorey (`understorey`)

Poly Haven's scanned fern, calathea and anthurium (`understoreyScanName`: `fern_02`, `calathea_orbifolia_01`, `anthurium_botany_01`; several plants a file), knee to chest high.

- **Loading** (`buildUnderstorey`): each glTF part placed by its node and stood on its foot (`footedPlant`: lowest point at y 0, centred); the kind's tallest brought to its height (`understoreyKindScale`, `understoreyHeight`: `fernHeight` 1.3, `calatheaHeight` 1.5, `anthuriumHeight` 1.7 m; the scans are house plants) and baked in. Without a scan, `understoreyStandIn` (three crossed blades, plain green).
- **Simplified** (`simplifyMesh`, meshoptimizer, within `understoreySimplifyError`): the seen mesh to `keepShare` (0.3) with its border locked, so a cut-out leaf keeps its outline; its level to `levelKeepShare` (0.12) and the shadow's stand-in to `shadowKeepShare` (0.05) without.
- **Where** (`placeUnderstorey` → `UnderstoreyPlacement`): its layer of the planting maps: 1.5 m cells, every one on the floor within 20 m of the path, far share 0.35 past 60 m, gap share 1.5, path clearance 1.5 m; trunk clearance 1 m past each trunk's reach (`floorTrunks`: 2 m round the big trees' buttresses, 0.4 m round a mid tree).
- **Kinds** by `fernShare` (0.45) and `calatheaShare` (0.3), the anthuriums the rest; each 75-125% of its kind's size (`understoreyScaleSpread`), sunk `understoreySinkMetres`, turned at random, a variant by `variantPick`.
- **Cards:** `impostorErrorMetres` 0.06, `impostorFrames` 8, `impostorTexels` 64: a plant is small, its card never big on screen.
- `ForestUnderstoreySettings` (F5 Forest, Understorey; start-up).

## Palms (`palms`)

Understorey palms 2-6 m tall grown by the engine (no CC0 palm scans).

- **A frond** (`frondSpine`, `PinnateFrond`, `appendPinnateFrond`): the rachis leaves the crown 35-72 degrees above level (the youngest, at the top, steepest) and bends down as a tapering cantilever under its own weight: curvature ∝ (1 - s)² / (1 - 0.7 s)⁴ (moment over stiffness, the radius tapering by `palmRachisTaper` 0.7), so the arch bends most in its middle (`frondBendShare`, closed form). The whole turn is `droopDegrees` (70) times 0.6 (youngest) to 1.4 (oldest). Fronds round the crown by the golden angle.
- **Leaflets** (`appendStrapLeaf`, `StrapFold`): `leafletPairs` (30) pairs past the bare `palmPetioleShare` (a quarter), lengths a spindle (`palmLeafletLength`, the longest `palmLeafletShare` 0.3 of the frond), widths `palmLeafletWidth`; each leaning 60 degrees forward, lifted 15 out of the frond's plane, folded 25 degrees into a V along its midrib (three vertices a row), drooping a fifth of its length; u across (0.5 on the midrib), v along. Opaque and double-sided: no cut-out test.
- **Stem and rachis:** tubes (`appendTube`) reading the texture's midrib column.
- **Variants** (`palmVariantShape` → `PalmShape`, `palmVariantHeight`): `variants` (8) evenly from `shortest` (2) to `tallest` (6) m, `leastFronds` to `mostFronds` (7-14) more on taller ones, fronds `palmFrondLengthShare` (0.45) of the height; grown on every core (`growPalmMeshes` → `PalmMeshes`, `growPalm` with a `PalmDetail`).
- **Thinned leaflet by leaflet** (`PalmDetail::thinned`, `PinnateThinning`; `scene/card_thinning`): the stem, stalks and every `palmKeepEvery`-th (4) pair's leaflets first (the stem and stalks their own anchors, so they stay), the rest after, shrinking away about their bases past random ranks while the kept grow by share^-`leafletGrowth` (0.25). The coarser level is the kept part, its error the gap the dropped pairs leave, then the card. The shadow's stand-in (a quarter of the pairs, 2 segments) is never seen.
- **Leaflet picture** (`palmLeafletImages` → `PalmLeafletImages`, `forestPalmLeafMaterial`): drawn at start-up, `forestPalmLeafletTexels` (256), `LeafSet024`'s mean colour (`meanColour`, alpha-weighted) tinted as the trees' leaves: a paler midrib, seven parallel veins a side, streaks, a darker edge, a browner dry tip; its height made a normal map (`normalMapFromHeight`).
- **Where** (`placePalms` → `PalmPlacement`): its layer of the planting maps: 5 m cells, every one on the floor within 25 m of the path, far share 0.3 past 60 m, gap share 2 (palms take a gap's light first), path clearance 2 m; trunk clearance 1 m; each 85-115% of its variant (`palmScaleSpread`), turned at random.
- **Cards:** `impostorErrorMetres` 0.15, 8 frames of 128 texels.
- `ForestPalmSettings` (F5 Forest, Palms; start-up).

## Ground cover cells (`ground_cells`)

`forestGroundCells(GroundCellOptions, jobs)` → `GroundCell {centre, step, variant, quarterTurns}`, shared by the grass and the floor's cover:

- Squares of `cellMetres` aligned on the origin, within `reachMetres` of the path; each with one paint channel's mean (`forestGroundPaint` at 4 x 4 points), stepped to the nearest of `densitySteps` (`groundDensityStep`; none under `leastPaint`); a variant and quarter turn from its place. A row of cells a job.
- `groundCoverLevelError(distance)`: the error covering the default pixel error at that distance in a 1080-pixel, 70 degree view.
- **Placing** (`placeGroundCover` in `forest_game`): every variant at every density step grown on every core (a sparser step the first of the same pieces), each given its half and quarter levels and nothing past the farthest; one still entity a cell, turned by its quarter turns, `followsGround` (each vertex lifted onto the land by the vertex shaders: `terrain.md`), casting no shadow (contact shadows and ambient occlusion ground it), swaying at the patch's height (at least 0.3 m).

## Grass (`grass`)

Blades in the open, on the grass channel's cells (`forestGrassCells`, `GrassCell`).

- **A blade** (`GrassBlade`, `appendGrassBlade`): a quadratic curve from its root through a point 0.6 of its height straight above to its leaning tip; rows closer towards the tip (t = 1 - (1 - s)^1.5), narrowing as 1 - t² to one tip vertex (4 segments: 9 vertices, 7 triangles). Normals its face's, rounded towards each edge and tilted up, so the field is lit from above whichever way a blade faces; its material keeps them on the back (`MATERIAL_BACK_KEEPS_NORMAL`). u across within its clump's tone band, v up.
- **A patch** (`growGrassPatch` → `GrassPatch`): `bladesPerSquareMetre` (150) over a `cellMetres` (8 m) square at full density, in a fixed random order; clumps on a jittered grid `clumpMetres` (0.8 m) apart, each blade taking its clump's height (`grassLeastClumpHeight` to `grassMostClumpHeight`, 70-120%), lean (10-45% of the height) and tone band; heights `shortest` to `tallest` (0.25-0.6 m), width `bladeWidth` (1.8 cm). Levels: half the blades widened by √2 in 3 segments, a quarter widened by 2 in 2, then nothing, at `halfDistance`, `quarterDistance`, `farthest` (15, 30, 60 m).
- **Cells:** `reachMetres` 200, `densitySteps` 4, `leastPaint` 0.05, `variants` 4 (`grassDensityStep`).
- **Picture** (`grassBladeImage`, `forestGrassMaterial`, `forestGrassBladeTexels`): drawn in the `Grass004` scan's mean colour, so blades match the ground they end on: `grassToneBands` (4) tones across, 55% at the root rising to full, drying to straw past 85% up, faintly streaked; opaque, double-sided, roughness 0.85, light through leaves.
- `ForestGrassSettings` (F5 Forest, Grass; start-up).

## Forest floor cover (`floor_cover`)

Under the canopy, on the leaf litter channel's cells (`forestFloorCells`), placed as the grass is.

- **Fallen leaves** (`FallenLeaf`, `appendFallenLeaf`): `leavesPerSquareMetre` (40) of `leafLength` (15 cm, 60-160%), an ovate outline 55% as wide round a middle point (7 vertices, 6 triangles), the midrib 4 mm up, edges curled up to 12 mm; normals up; a picture band each, one in ten green.
- **Seedlings** (`Seedling`, `appendSeedling`): `seedlingsPerSquareMetre` (1.5), `shortestSeedling` to `tallestSeedling` (0.1-0.4 m), a 3 mm stem from 2 cm underground, 2-6 strap leaves (`appendStrapLeaf`) round its top half by the golden angle.
- **Small ferns** (`FloorFern`, `appendFloorFern`): `fernsPerSquareMetre` (0.4), 4-7 fronds of `fernLength` (0.5 m, 80-100%) by `appendPinnateFrond` (10 leaflet pairs, rising 50-70 degrees, arching over).
- **A patch** (`growFloorPatch` → `FloorPatch`): each kind in its own fixed random order; levels of half the leaves and every plant, then a quarter of the leaves and half the plants, then nothing, at 12, 25 and 50 m.
- **Picture** (`floorCoverImage`, `forestFloorCoverMaterial`): `floorCoverBands` (4) leaves side by side in bands of u: the litter scan's mean colour darker, as it is and yellowed, and the trees' leaves' green (`floorGreenBand`: seedlings, ferns, fresh leaves); midrib, veins, mottling, darker edges. Opaque, double-sided, back keeping its normal, roughness 0.8, light through leaves.
- Fallen leaves lie at no height, so they never sway; the patch bends as the floor's sway group.
- `ForestFloorSettings` (F5 Forest, Forest floor; start-up).
