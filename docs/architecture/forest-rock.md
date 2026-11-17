# Forest: rock, islands and what hangs from them

Covers: `src/games/forest/valley_strata`, `src/games/forest/valley_shapes`, `src/games/forest/island_hangings`, `src/games/forest/hanging_moss`, `src/games/forest/drifting_rocks`

The valley's pillars, floating islands, the Ring, the Monolith and far towers, the strata painting them, and the roots, vines, moss and drifting rocks round them. The game: `forest.md`.

## Strata (`valley_strata`)

One layer cake for the whole valley (`valleyStrata()` → `StrataBand {bottom, top, hard, tone}`), from `valleyStrataBottom` (-400 m) to `valleyStrataTop` (1400 m), the same every run. Eroded flat beds leave the same bed at the same height on every pillar, so the eye joins the bands across kilometres; the islands show the upper beds.

- Soft sandstone beds 15 to 60 m thick (mostly thinner), each a tone between the sandstone's two tints, at least 0.3 from the soft bed below. About one bed in five hard (`hard`), 5 to 15 m of big blocks, never two together: the ledges.
- `strataBandAt(bands, height)`: the bed at a height.
- `strataImage(bands, texels)`: the strip the shader paints by world height (4096 texels over the cake, about 0.44 m each): each texel's share of each layer is the share of its height the beds cover, a soft bed split between the two tints by its tone.
- `strataWarpImage`: seamless value noise (four octaves, 256 texels) repeating every 1.5 km, moving the beds up to `ForestRockSettings::warpMetres` (6 m) either way.
- `streakImage`: seamless water streaks, clean below 0.55, darkening to 0.15 by 0.75, a repeat every 24 m across and 300 m down.
- `toneVariationImage`: seamless patches repeating every 400 m.

## The rock material (`rockMaterial` in `forest_game`)

Every pillar and island shares it: `forestRockLayers` projected along each shape's own axes in metres, painted by the strata strip (`GroundPaint::Strata`), the moss on what faces up. Engine side: `parallax-and-grounds.md`.

| Layer | Scan (Poly Haven) | Shown |
|---|---|---|
| Sandstone | `cliff_side` | 4 m |
| Sandstone, greyer and darker | `cliff_side` times `ForestGroundLayer::tint` | 4 m |
| Hard beds' blocks | `marble_cliff_03` | 12 m |
| Moss (`forestMossLayer`) | `aerial_rocks_02` | 50 m |

`ForestRockSettings` (F5 Forest, Rock; live but `warpMetres`): blend depth 0.3, moss from 0.55 to 0.8 of the normal's upward part, bed waves 6 m, streaks 0.45, hex tiling on, tone variation 0.25. `applyRockSettings` writes them only when changed.

## Carved shapes (`valley_shapes`)

Every pillar and island is its own mesh at its real size, in metres, built on the job system and kept drawn-only with levels (`addDrawnMeshWithLevels`: a quarter and 6% of the triangles within 2% of its size, `rockLevelShare`, `rockFarLevelShare`, `rockLevelError`). Each placement carries its own `seed`.

- **Grids:** `surfaceGrid(rows, columns, wrapRows, point)` → `SurfaceGrid`: rows up, columns round, the last column joined to the first (and the last row to the first for a torus). `wrappedSurfaceMesh` makes a `Mesh`; `wrappedSurfaceIndexedMesh` the render format directly (one vertex a point, tangent round), which the carved shapes use, since converting millions of triangles through `Mesh` was slow. Triangles wind so "around" crossed with "up" points out; zero-area ones (at a pole) are dropped; normals are area-weighted.
- **Resolution:** a row every `rockRowMetres` (2.5 m) up, a column every `rockColumnMetres` (2.5 m) round (24 to 160 round a pillar).
- **Carving** (`RockCarving`: the beds, the warp texture, its tile and height, the joints' depth):
  - Ledges (`strataRelief`): a hard bed stands out `rockLedgeMetres` (2 m); soft rock within `rockUndercutReach` (4 m) under one is cut back up to `rockUndercutMetres` (1.5 m), so each ledge throws a shadow line. Beds are waved as the shader waves them (`carvingWarp` reads the same texture), so bed waves are a start-up setting.
  - Joints (`jointGroove`): vertical cracks `rockJointDepth` (2.5 m) deep about every `rockJointSpacing` (9 m) round, fractal noise read round a circle, wandering as they rise.
- `pillarSurface(placement, carving)`: `pillarBuriedDepth` (5%) of its height buried, tapering by a third, bulging by noise read round a circle (so the seam matches), its axis leaning by slow noise, then beds and joints in metres, never under a third of the plain radius; a broken, nearly flat top 1.6 m above the body, room for trees. The top's last row lies exactly on the axis.
- `islandSurface(placement, carving)`: a jagged underside narrowing from the rim to a point `islandDepth` (1.4) radii below, carved and fading out towards the tip; a gently domed top.
- `ringMesh(tubeShare)`: a unit torus round z.

## Placements

| What | Function | Numbers |
|---|---|---|
| Pillars | `placePillars(ValleyLayout)` | dart throwing (Poisson disk), `pillarCount` (220); 200-350 m tall, 12-45 m in radius, thickest near the axis; 70 m of air between two; clear of the path (40 m plus a third of the radius), the Ring, the clearing, the Monolith and the start; each on the lowest ground under its edge |
| Monolith's pillar | `monolithPillar` | appended to the pillars; the bridge's far end on the western rim |
| Islands | `placeIslands(layout, pillars)` | `islandCount` (30); 25-150 m radius, tips 380-700 m over the ground, none over a pillar reaching within 120 m of their tip, none within 400 m of the start |
| Ring | in `buildShapes` | `forestRingRadius` 200 m, tube `forestRingTubeRadius` 12 m, steel, at `forestRingPlace` across the path, its centre on the ground (half buried) |
| Monolith | `monolithBoxes(groundLevel)` → `ValleyBox`es | a 600 m slab across the clearing's north end (front at `forestMonolithPlace`), lower blocks either side, a crown with masts, a deck east, a truss bridge west; cubes scaled by `halfSize`, concrete, static and walked against |
| Far towers | `farTowerBoxes` | ten slabs 300-800 m tall, 2.5-7 km north in the haze |

Pillars, islands and the Ring are still but not static: no collision, not in the sky visibility's level.

## Strands, tubes and cards (`island_hangings`)

Shared by everything that hangs or is a tube: the islands, trees, lianas, climbers.

- `HangingStrand`: points from the anchor down, a radius each, and optional `HangingVertex` entries for the wind. `hangingStrand(anchor, outward, length, startRadius, tipRadius, seed)`: points `hangingStepMetres` (1 m) apart, leaving level and turning down as a heavy rope (the share pointing down 1 - exp(-s / 4 m)), wandering sideways by smooth noise, never climbing; radius falling as (1 - s / length)^1.5.
- Wind (`wind.md`): `hangFrom(strand, anchor)` (each point's arm its length along; a fixed anchor for rock, `windAnchorOwn` for a thing on a swaying object); `hangBetween(strand, first, second)` (share along, arm the sag below the chord); `strandHangingAt` blends entries between points.
- `appendTube(mesh, strand, sides, tileMetres, options)`: a ring of sides + 1 vertices at each point, its frame (`tubeFrames`, `TubeFrame`) carried ring to ring so it never twists; v along in tiles; tangent round, bitangent down. `TubeOptions`: `wholeTilesRound` (u spans a whole number of tiles round the first ring) and `TubeLobes` (buttresses: `lobedRadius` = r (1 + exp(-max(y - ground, 0) / h) Σ a max(0, cos(θ - θ_j))^p), each fin's angle and height jittered; a lobed tube's normals from its own differences).
- `ClimbHost`: a trunk's strand, lobes and fork height, kept for climbers (`forest-trees.md`).
- Leaf cards: `appendLeafCard` (square), `appendOutlinedLeafCard` (a card drawn as a cut outline, `squareOutline` the plain one), `atlasCell`/`AtlasCell`, and `appendLeafCards(mesh, vine, spacing, size, seed)`: ivy cards about every `spacing` along a vine, 1.5 times as wide as long (the cells are 3:2), stems on the vine, pointing out at random and drooping.

## Island hangings

`islandHangings(island, surface, settings)` → `IslandHangings` (bark and leaves meshes, tips and their arms for the moss, counts), built in the island's own job and frame and placed with its rock; two drawn-only meshes an island, swaying as hanging things.

- `rootsPerTenMetres` (3) roots and `vinesPerTenMetres` (2) vines per 10 m of radius, anchored on the underside (`islandUnderside`: below a fiftieth of the radius under the rim), more often near the rim (`towardsTheRim`: the square root of a random share), sunk 0.2 m into the rock, each `hangFrom` a fixed anchor.
- Roots: 20-100% of `longestRoot` (50 m), radius 0.15-0.75 m at the rock tapering to 4 cm, 8 sides. Vines: 20-100% of `longestVine` (80 m), radius 6 cm to 3 cm, 4 sides, with ivy (`leafSpacing` 0.5 m, `leafSize` 0.4 m).
- Bark: Poly Haven `bark_brown_02` in 2 m tiles (`forestBarkLayer`); ivy: ambientCG `LeafSet017` (`forestLeafMaterial`).
- `ForestHangingSettings` (F5 Forest, Hangings; start-up).

## Hanging moss (`hanging_moss`)

No CC0 library has any, so it is drawn at start-up; one drawn-only mesh a shape, placed as the shape, swaying as hanging.

- **Texture** (`mossStrandImage`, `forestMossTextureWidth` x `forestMossTextureHeight`, 512 x 1024; `forestMossMaterial`): about 0.6 strands a texel across hanging from the top edge, each a wavy line 1-3 texels wide thinning to its tip, 15-100% of the height (a squared random share: most short), anti-aliased into alpha; colour the `Moss001` scan's mean (`forestMossStandIn` without it), each strand 0.65-1.15 of it and darker at the top; alpha 0 between strands over the same colour; coverage-keeping mips.
- **A clump** (`appendMossClump`): two crossed vertical cut-out cards under its top point, turned at random, `mossClumpWidthShare` (a third) of its length wide; each vertex hangs from a fixed anchor, its arm its depth below the top.
- **Lips** (`overhangLips` → `MossPlace`): surface points standing out at least `mossLipMetres` (1 m) past the point below: the ledges' lower edges and where a joint's groove ends.
- **Where** (`ForestMossSettings`, F5 Forest, Moss; start-up): under `ledgeShare` (0.35) of each pillar's lips (`pillarMoss`); on `tipShare` (0.7) of each island's root and vine tips at half length, and `undersidePerTenMetres` (6) per 10 m of radius over the underside (`islandMoss`); each 30-100% of `longestMoss` (6 m), its top tucked 0.1 m into the rock.

## Drifting rocks (`drifting_rocks`)

Small rocks floating round the islands and some pillars, bobbing and turning; each its own drawn-only mesh with levels, casting shadows, not still.

- **Component** `DriftingRock`: rest, size, bob (`bobMetres` either way over `bobSeconds`, from `phase`), turn (`turnRadians` + `turnSpeed` t, up to 0.04 rad/s either way), orbit (round the vertical through `orbitCentre` at `orbitSpeed`, facing turned with it), `seed`.
- `driftingRockPose(rock, seconds)`: its `Transform` (doubles, wrapped); `updateDriftingRocks(world, simulationSeconds, moving)` sets it each tick from the simulation's clock (time 0 while not `moving`), as `Spinner` does; a `PreviousTransform` interpolates it.
- **Where** (dart throwing, each clear of the others by both sizes and bobs): `islandRocks` (`islandRocksPerTenMetres`, 1, per 10 m of radius, 1.35-1.95 radii out, down to 0.6 radii below the rim); `pillarRocks` (on `pillarShare`, 0.3, of pillars, one to three, 1.7-2.3 radii out at 45-90% of the height). `valleyRocks` drops any touching another shape anywhere round its orbit (`nearestToAxis`) and checks rings of rocks round different shapes against each other where their heights meet (`ringsClear`). Sizes 10-100% of `largestRock` (20 m), mostly small.
- Every rock round one shape orbits at the same rate (`orbitMetresPerSecond` along the ring's middle, each ring its own way), so a ring turns whole and its rocks keep their distances.
- **Shape** (`driftingRockSurface`): an island's at the rock's size, uncarved by joints (`RockCarving::jointMetres` 0).
- **Rock:** the rock material with its strip replaced by one soft bed's tones, since world-height beds would slide over a bobbing rock; moss and streaks are projected in its own frame, so they ride with it. The Rock settings apply to it each frame.
- **Shadows:** the far shadow map draws moving casters over a cached copy of the still ones (`lights-and-shadow-atlas.md`).
- `ForestDriftSettings` (F5 Forest, Drifting rocks; start-up but `moving`): bob 12 m over 15 s, orbit 3 m/s.
