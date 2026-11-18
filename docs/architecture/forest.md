# Forest

Covers: `src/games/forest/forest_game`, `src/games/forest/forest_terrain`, `src/games/forest/forest_ground`, `src/games/forest/forest_sway`, `src/games/forest/structures`

`ForestGame` (`--game forest`, title "Forest"): a jungle valley at a scale meant to be felt. Rules for working here: `src/games/forest/CLAUDE.md`; research: `docs/research/JUNGLE.md`. North is -z.

Other forest files: `forest-rock.md` (pillars, islands, what hangs from them, drifting rocks), `forest-trees.md` (trees, lianas, climbers, epiphytes), `forest-plants.md` (understorey, palms, grass, floor cover).

## Start-up (`onInit`)

In order: `buildGround`, `buildShapes`, `buildTrees`, `buildLianas`, `buildUnderstorey`, `buildPalms`, `buildGrass`, `buildFloorCover`; then the start, the body, the sun, the air entity and `ProbeCascades`.

- Generated meshes are built on the game's job system (`Game::getJobs`), each into its own slot, so the result does not depend on thread order; most are kept drawn-only (`AssetManager::addDrawnMesh`).
- Scenery is `ModelRenderer::isStill` (`addShape`): its draws are kept between frames, so any change to its material or sway is announced with `markRenderChanged`.
- Static and walked on (`StaticLevel`): the finest ring of ground and the Monolith's slabs and strut only. Pillars, islands, the Ring and trees have no collision; they stand clear of the path.
- Without `assets/forest` (a fresh clone; tests pass a folder that does not exist) every texture role falls back to a plain stand-in, and drawn textures use stand-in colours.

## The land (`forest_terrain`)

Pure functions of the place (`forestHeight`, `forestNormal`, `forestGroundPaint`), so the drawn ground, the walked ground and the paint agree, and tests can ask any point.

| Feature | Shape |
|---|---|
| Floor | height 0 within `valleyFloorHalfWidth` (200 m) of `valleyAxis` (straight along the walk, swinging up to 400 m either way north of z = -700) |
| Walls | climbing to `valleyRimHeight` (150 m) over `valleyWallWidth` (150 m) |
| Ramp | the floor climbing south from z = 50 to the rim at 750, 70% a straight slope and 30% eased; the start (`forestStartPlace`, (0, 790)) on the rim at its top |
| Mountains | rising over 300 m to 2.5 km outside the valley to 60-100% of 700 m, by noise about 2.5 km across |
| Small shapes | fractal value noise everywhere: hills about 100 m across (9 m high), knolls 28 m (7 m), lumps 6 m (0.25 m) |
| Clearing | level, `forestClearingRadius` 120 m round `forestClearingCentre` (0, -330), blending over 60 m; the Monolith's foot |
| Path | `forestPathPoints`: from the start down the ramp in wide bends, through the Ring and along the floor to the clearing; level across within `forestPathHalfWidth` (1.5 m), blending over 3 m, sunk 8 cm; along it the land averaged over 30 m either way (`forestPathDistance` for distances to it) |
| Openings | `forestMeadowShare`: 1 in the clearing, the meadow round the start (`forestMeadowRadius`, 60 m) and wide openings where low-frequency noise is high |

- **Noise:** `forestValueNoise` (hashed lattice, quintic fade, so slopes are continuous) and `forestFractalNoise` (octaves each twice as fine, half as strong and turned their own way, so the sum has no square grain).
- **Random numbers:** `forestUnitRandom(seed, index)` and `forestShapeSeed(seed, use)` (a seed per use of a shape's seed) are the forest's only random numbers.
- **Rings** (`TerrainRing`, `forestTerrainRings(nearSpacing)`): nested squares round `forestTerrainCentre` (0, 170, halfway along the walk). Half sizes 640 m (`forestNearHalfSize`), 2.56, 10.24 and 20.48 km; squares 1, 8, 32 and 128 times the finest. The finest is 2, 4 or 8 m (`ForestValleySettings::groundCoarseness` 0-2: `2 << coarseness`); others throw. `forestRingHeight` makes each ring's outer border follow the next ring's coarser edge.
- **What the rings are now:** ring 0's mesh (`forestTerrainRingMesh`, rows on the job system) is the ground walked on and the sky visibility's static ground, `hidden` and casting no shadow; every ring's square is a terrain region with its own material. The coarser rings are never meshed.

## The terrain drawn

A `Terrain` (CDLOD; engine side: `terrain.md`) on its own entity:

- Height grids from `forestHeight` (`makeTerrainHeightGrid`): the finest spacing over ring 0, four times coarser over ring 1, sixteen times over the root's square.
- Leaves of `TERRAIN_GRID_QUADS` (32) finest squares (64 m at 2 m), as many levels as reach past the land's edge from the centre (11 at 2 m: a 65.5 km root); `landHalfSize` the last ring's.
- Regions: each ring's square and its ground material, the finest first.

## The ground's paint (`forest_terrain`, `forest_ground`)

- **Layers** (`forestGroundLayers`, weights channels `forestGrassLayer`, `forestLitterLayer`, `forestMudLayer`, `forestRockLayer`): ambientCG `Grass004` (2 m tiles), Poly Haven `leaves_forest_ground` (2.5 m), `brown_mud_leaves_01` (2.5 m), `mossy_rock` (3 m).
- **Paint** (`forestGroundPaint`, slope over the texel's size): mud on the path, mossy rock where the ground is steeper than about 23 degrees (all of it past 33: the walls), then grass where the canopy is open and leaf litter where it is closed, their edge ragged by fine noise.
- **Weights** (`forestWeightsImage` over a `GroundExtent`, a row a job): 2048 texels over ring 0 (`forestFinestPaintTexel`, 0.625 m), 1024 over each coarser ring's square; halved per step of `groundCoarseness`.
- **Blend** (`forestBlendDepth`): the setting's depth on the finest paint, deeper by the square root of how many times coarser the texels are, at most 1, so coarse texels' corners do not show as squares. Each region repeats its layers by its own square (`groundRepeats`). Blended grounds themselves: `parallax-and-grounds.md`.
- **Files** (`findForestTextureFiles`, `ForestTextureFiles`): colour, normal, height, and Poly Haven's packed occlusion-roughness-metal or ambientCG's separate roughness and occlusion, and an opacity for cut-outs. `forestLayerMaterial` loads a `ForestGroundLayer` from `forestAssetFolder` or gives its plain stand-in (colour, roughness).
- **Leaf materials:** `forestLeafMaterial` (an atlas as a double-sided cut-out, opacity in alpha, mips keeping coverage; the ivy `LeafSet017` by default), `forestLeafClusterMaterial`, and drawn ones (palm, aroid, epiphyte, grass, floor cover, moss), each with its plants.
- **Light through leaves** (`ForestLeafLightSettings`, F5 Leaf light, start-up; `withLeafLight`): `Material::diffuseTransmission` share 1, tint (0.8, 1, 0.4), on every leaf material; `engine.render.materials.lightThroughLeaves` switches it live.

## The structures (`structures`)

Stealth-alien shapes (design: `docs/research/STRUCTURES.md`), every one lofted from sections by `appendLoft`: convex polygons of equal size whose points go round from -u through +v to +u and -v (u x v back along the loft), so the sides face out; both ends capped; vertices shared, triangles flat (no corner normals). Each triangle's corners share one texture coordinate, two `forestUnitRandom` numbers, read as its facet's tone and roughness (`Material::facetTone`, `facetRoughness`; `materials.md`).

| Shape | Built by | Form |
|---|---|---|
| Slab | `makeSlab`, `slabMesh` | hexagon sections (left edge, front ridge, a knee, right edge, the same turned half round: three facets a side a band; `ridgeShare` across, zigzagging by each level's `ridgeShift`), bands every 60-120 m on the Monolith: sunk 10 m, nearly upright to `shoulderShare` of the height, a shoulder stepping in a quarter over `slabShoulderRise` (3 m, so its facets face the sky: upward part 0.55-0.9, the rock's moss range), a chisel top cut towards `cut`'s side |
| The Monolith | `monolithSlabs` | five slabs 240-600 m tall, 79-110 m wide, depth 0.45 of width, tops cut alternately; the tallest's front ridge on `forestMonolithPlace`, the others staggered 10-20 m behind |
| Strut | `monolithStrut`, `strutMesh` | an 8 m rhombus from inside `monolithPillar` into the second slab's side, 331 m up |
| The Ring | `ringSegmentsMesh` | `ringSegments` (20) straight prisms round a polygon, middles `forestRingRadius` out, chamfered square section of half `forestRingTubeRadius`, ends cut on radial planes 1.5 m short; within 30 degrees of the top drifted 3-10 m out and turned up to 1 degree. One mesh, at the ground with no scale |
| Far spires | `farSpires` | ten needles 300-800 m at the old towers' places, sunk 20 m |

- One material for all (`applyStructureSettings`, `ForestStructureSettings`): matte coating, linear albedo 0.04 times a cool tint, roughness 0.65, facet tone 0.12, facet roughness 0.07; rewritten when the settings change (`appliedStructureSettings`).
- `getStructures()` lists every structure entity; `getMonolith()` its slabs and the strut (static, with collision).

## Sky, air and wind

Everything on the air entity, rewritten each frame by `updateDaylight`:

- **Sun** (`ForestSunSettings`): 5 degrees north, day 80, 9.5 h, still clock (`advanceSolarHours` in ticks), intensity 4 above the air, disc 5 times real.
- **Atmosphere:** its ground the leaf litter's stand-in colour, so the planet past the land matches.
- **Mist** (`ForestMistSettings`): a `Fog` 0.0005 per metre on the floor (about 6 km of sight), thinning by e every 300 m up, out to 20 km.
- **Wind:** a `Wind` blowing down the valley (towards 270 degrees, 3 m/s; F5 Forest, Wind, live). The field: `wind.md`.
- **Far plane** 25 km (reversed-Z keeps it precise).

## Sway (`forest_sway`)

- `ForestSwaySettings` (F5 Forest, Sway, live): a `ForestSwayGroup {bend, flutter}` each for trees (0.1, 0.3), plants (0.3, 0.04), grass (0.5, 0.02) and the floor's cover (0.3, 0.01), plus `hangingSwing` (0.3) and `hangingFlutter` (0.05).
- Each swaying entity is a `ForestSwayer {entity, part, height, leafy}`: its `ForestSwayPart`, the height its bend is measured against (a tree's for everything on it), and whether it has leaves to flutter.
- `forestSway(settings, swayer)` → `WindSway`; `applySway` gives every swayer its sway only when the settings change, marking each changed, and writes the lianas' `WindAnchors` (their bend the trees').

## Settings

F5 Forest groups: Sun, Atmosphere, Wind, Sway, Mist, Ground, Rock, Structures, Hangings, Moss, Drifting rocks, Canopy trees, Emergent trees, Mid trees, Leaf light, Understorey, Palms, Lianas, Climbers, Epiphytes, Grass, Forest floor, Valley.

- Live: Sun, Atmosphere, Wind, Sway, Mist, Ground (`ForestGroundSettings`: blend depth 0.2, hex tiling on), Rock (but its bed waves), Structures, drifting rocks' `moving`.
- Start-up: the rest, including `ForestValleySettings` (`groundCoarseness` 0, `pillarCount` 220, `islandCount` 30, `start`).
- Ground and rock materials are rewritten only when their settings differ from those last applied (`appliedGroundSettings`, `appliedRockSettings`).

## Start, views and commands

- **Start** (`ForestValleySettings::start`, `forest.valley.start`): `ForestStart::Rim` (the default, facing north) or `Trees` (`forestTreesStart`, `forest-trees.md`), set before the body spawns (`PlayerControls::setFirstPersonFeet`).
- **Views** (`forestViews()`, `ForestView`): `rim` (from the start towards the Monolith), `ring` (the Ring from the path 230 m south), `wall` (up the Monolith from its foot).
- **Commands** (`addCommands`): `forest-view <name>` poses the free camera at a view; `forest-climber [n]` and `forest-epiphyte [n]` (`forest-trees.md`).
- **Golden views:** `rim`, `ring`, `wall` at 9:30 and `rim-dusk` at 17:48.

## Tests

`forest_test` (the land, rings, paint, weights, layer files and stand-ins, strata, shapes, the headless game at its coarsest ground, `forest-view`, settings applied); one test file per other module (`forest_structures_test`, `forest_trees_test`, `island_hangings_test`, `hanging_moss_test`, `drifting_rocks_test`, `understorey_test`, `palms_test`, `lianas_test`, `climbers_test`, `epiphytes_test`, `grass_test`, `floor_cover_test`), sway in `wind_sway_test` and `wind_hanging_test`. Shared helpers: `tests/forest_helpers.h`.
