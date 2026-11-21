# Planting from maps

Covers: `src/scene/planting`, `src/scene/planting_maps`, `src/scene/planting_shared.h`, `assets/shaders/planting.glsl`, `assets/shaders/hash64.glsl`, `src/core/hash_shared.h`, `src/scene/planting_cells`, `src/render/gpu/planting_records`, `src/render/gpu/planting_records_shared.h`, `src/render/vulkan/vulkan_planting`, `assets/shaders/planting_cells.glsl`, `assets/shaders/planting_candidates.comp`, `assets/shaders/planting_spacing.comp`, `assets/shaders/planting_write.comp`, `src/render/gpu/placed_drawing`, `src/render/gpu/placed_drawing_shared.h`, `src/render/vulkan/vulkan_placed_drawing`, `assets/shaders/placed_drawing.glsl`, `assets/shaders/placed_plant.glsl`, `assets/shaders/placed_cull.glsl`, `assets/shaders/placed_cull_count.comp`, `assets/shaders/placed_cull_scan.comp`, `assets/shaders/placed_cull_commands.comp`, `assets/shaders/placed_cull_write.comp`, `assets/shaders/drawn_objects.glsl`

Plants placed from maps by an ordered dither, a box at a time, the same on the CPU and the GPU (Horizon Zero Dawn's placement; research and the user's choices: `docs/research/VALLEY.md`). A game builds the maps and turns its settings into layer records; the forest's: `forest-plants.md`. Stage 2b1: the GPU plants cells round the camera and `placement-compare` checks them; 2b2: the camera's prepass and main pass draw the kinds a game gives looks; e1: three sets of cells, each layer planted only as far as it is drawn.

## Exact on both sides

| Rule | Why |
|---|---|
| Adds, multiplies, compares and whole numbers only | a GPU may round division, square roots and trigonometry differently; Vulkan rounds add, subtract and multiply correctly |
| Reciprocals and turns made on the CPU and passed in (`inverseTexel`, `inversePathSpan`, `pathInverseLengths`, the ring's directions) | so both multiply by the same number |
| `precise` on every GLSL result; `-ffp-contract=off` on the C++ files | no multiply and add fused into one rounding |
| Distances compared squared (`plantingWithin`) | no square root |
| The 64-bit hash (`mixBits`, `hash64.glsl`, `shaderInt64` required) | the CPU's random numbers unchanged |
| Map texels read raw and blended in code (`terrainBilinear`) | hardware filtering keeps 8 bits of the blend |

`planting_gpu_test` checks every function bit for bit, its cases on the decisions' edges (a radius equal to the distance, a threshold equal to the share) as often as off them.

## The maths (`scene/planting`, mirrored by `planting.glsl`)

| Function | Does |
|---|---|
| `plantingRandom(seed, index)` | 24-bit unit random number, SplitMix64 twice (`forestUnitRandom` is it) |
| `plantingThreshold(column, row)` | Roberts' R2 ordered dither, 32-bit fixed point |
| `plantingShare` | the floor's, wall's and rim's blended weights (0..255) times the layer's share a unit of weight, its gap thickening or thinning, the path's share; clamped 0..1 |
| `plantingPathShare` | 1 within near, farShare past far, straight between |
| `plantingTaken(threshold, below, share)` | the band from `below` (a layer sharing the lattice) to below + share |
| `plantingJitteredPlace`, `plantingScale`, `plantingTurn`, `plantingUniformKind`, `plantingSharedKind` | a cell's plant from its own random numbers (`PLANTING_DRAW_*`: scale, turn, kind, pick after the two placing it) |
| `plantingCoverKind`, `plantingCoverTurn`, `plantingCoverValue` | a cover layer's byte to its kind and quarter turns and back (`PLANTING_COVER_TURNS`, `PLANTING_QUARTER_TURN`) |
| `plantingNearSegment` | closer than a radius to a piece of path (nearest point, squared) |
| `plantingRingPoint` | eight points round a trunk (the lowest ground) |
| `plantingTop`, `plantingFits`, `plantingGivesWay` | a sphere under the caps and clear of obstacles; a plant giving way to a nearer earlier one of its layer |
| `plantingStored`, `plantingPathDistance`, `plantingCoordinate`, `plantingBetween` | stored whole numbers to metres, a place on a grid, a straight blend |

## The maps (`PlantingMaps`)

A grid across a winding axis (row r's middle at northmost + (r + 0.5) texel, the axis at `axis[r]`, read between rows in straight lines):

| Map | Holds | Read |
|---|---|---|
| `ecotopes` | four bytes summing to 255: floor, wall, rim, bare | blended; all bare outside |
| `gaps` | 0..255 | blended; none outside |
| `path` | distance to the centre line, centimetres | blended, for the density's blend only |
| `pathPoints`, `pathInverseLengths` | the path's straight pieces | `plantingClearOfThePath`: exact, for its clearance |
| `clearance` | decimetres to the nearest obstacle at a texel's middle (at most `plantingFarthestClearance`) | nearest texel less `texelReach`: a lower bound |
| `caps` | a reach each, smallest first, square to the world; decimetres above -1000 m | the smallest at least the reach; `plantingUncapped` where no line passes |
| `coverMaps` (`PlantingCoverMap`) | a cover layer's byte a cell of its own lattice: 0 nothing, else 1 + kind x 4 + quarter turns (`plantingCoverValue`), baked by the game | `plantingCoverAt`: exact; 0 outside |

A signed path distance was tried: blended straight across the line, but the sign flips past the path's ends, drawing false lines of "on the path" across the valley; the unsigned map blurs the centre line's V by up to a metre, so clearance tests the pieces themselves.

## Layers (`PlantingLayerRecord`)

| Field | Meaning |
|---|---|
| `sharePerWeight`, `gapStep`, path blend | the density, from `PlantingDensity` (plants a hectare a ecotope, footprint, jitter, gap share, path) by `setPlantingDensity` |
| `footprint`, `jitterMetres`, `pathClearance`, `seed` | the lattice and each plant's own random numbers |
| `under` | an earlier layer on the same lattice: this one takes the thresholds above its share, never its cells |
| `kinds`, `kindShares`, `reachPerScale`, `centrePerScale` | alike or by cumulative shares (at most 5); each kind's sphere a unit of scale (one entry: all alike); at most 32 kinds |
| `leastScale`, `scaleRange`, `footRingPerScale`, `footSink` | size; the lowest ground on a ring round the trunk (0: at the foot), sunk |
| `spacingShare` | gives way to a candidate of its layer with a lower threshold (the earlier cell on a tie) nearer than this share of the larger reach |
| `keepAway` | earlier layers' plants it stays clear of: within a radius of their trunks, or their own reach (`crown`) |
| `cellSet` | the GPU's set of cells planting it (`plantingCoarseCells`, `plantingMiddleCells`, `plantingFineCells`); the CPU ignores it |
| `coverMap` | a cover layer's map (-1 none): exactly the cells it names planted, at their middles on the ground, kind and turn from the byte (`plantingCoverKind`, `plantingCoverTurn`: whole numbers and quarter turns, exact both sides), scale and pick drawn as any layer's; no dither, shares, path, keep-aways or fits; may not share a lattice or keep away (`checkPlantingLayers`); its map must exist, be its size and name only its kinds (`checkPlantingCover`) |

## Planting a box (`plantLayers`)

1. Backwards (`plantingLayerBoxes`, shared with the GPU's cells): each layer's box is the asked one joined with what every later layer reads of it (that layer's candidates' box grown by the keep-away's reach and two cells of each lattice); its candidates' box is its own grown by its spacing's rings plus a cell.
2. Forwards, a layer at a time: candidates on the jobs, a row each (dither, path, keep-aways by lattice cell, the ground from the terrain's drawn surface `terrainSurfaceHeight`, the maps); then the spacing; kept plants by cell for later layers, those in the asked box returned in row order.

- **A box alone equals the whole:** any box's plants are exactly those a larger planting puts in it, cut any way and planted in any order (`testABoxAloneIsTheSameAsTheWhole`). A lookup past what an earlier layer planted throws `std::logic_error`.
- **Cost:** grows with the box and its borders (a layer's reach and its spacing's rings), not the world; a plant costs a few map reads, a hash, nine ground reads for a tree.
- Tests: `planting_test` (maths, map reads, layers, boxes, shared lattices), `planting_gpu_test` (`make gputest`, bit for bit).

## Cells on the GPU (`gpu-placement` 2b1)

A world's `PlantingSource` (a `PlantingField`: maps and layer records; the forest's on its air entity) reaches the renderer as `FrameDescription::planting`.

| Piece | What |
|---|---|
| `PlantingCellSettings` (F5 Render > Planting) | off by default while nothing draws; a group a set (`PlantingCellSetSettings`: cell, radius, cells a frame; dropped a cell past the radius): coarse 128 m to 1 km, middle 64 m to 300 m, fine 32 m to 100 m, 4 a frame each (Horizon's 128 m tree blocks, 32 m grass) |
| `plantingSetLayers` | a set's layers: those naming it and all they read (keep-aways, `under`), in order, renumbered; `fieldLayer`, `written` (its own; the rest planted only to be read) |
| `PlantingCellRing` | a set's cells held and their slots: drops first, then the cells within the radius not held, nearest first (then row, column), each into the lowest free slot; a new size or radius starts again; ordered by cell, never address |
| `plantingSlotCount` | the cells touching twice the drop distance a side: 19 x 19 = 361 coarse, 13 x 13 = 169 middle, 10 x 10 = 100 fine |
| `PlantingSlotLayout` | a slot: each written layer's lattice over a cell (`plantingLatticeCapacity`; none for a read one); a cell made: each layer's lattice over its candidates' box |
| Records (`planting_records`) | maps header 96 B + words (bytes and halves packed; cover maps `PLANTING_COVER_MAP_WORDS` each, then their bytes), `GpuPlantingLayer` 448 B (`coverMap` among them) (keep-aways' reaches and spacing rings from the CPU), `GpuPlantingJob` 80 B a layer of a cell made (its three lattices, box, places from its set's `PlantingSetPlaces`), candidate 48 B, `GpuPlacedPlant` 32 B |
| `VulkanPlanting` [PLANTING] | uploads the field when it, its version or any set's settings change (every cell made anew): the maps once; a set planting a layer its layers' records, jobs, ring; every set's slots one after another in one plants and one counts buffer (`PlantingSetPlaces`), the candidates' scratch the largest set's; after WIND |

A set at a time, three passes a layer over its frame's cells (a workgroup row a cell), in layer order, a barrier between each:

1. `planting_candidates`: every cell of the candidates' lattice written (`plantingCandidateAt`, `candidateAt` line for line), kept cleared; earlier layers' kept plants read through their job of the same cell.
2. `planting_spacing`: over the standing lattice, kept unless it gives way; reads only neighbours' candidate marks.
3. `planting_write` (after every layer, the set's written ones only): a workgroup a cell writes the kept plants in its box in rows from the north by a 256-wide prefix sum, no atomics, and the count.

- **Same plants, same order as `plantLayers`:** the map reads (`planting_cells.glsl`) mirror `planting_maps.cpp`; the ground is `terrainSurfaceHeight` in `terrain_heights.glsl`, by the grid's reciprocal spacing from the CPU (`GpuTerrainGrid::inverseSpacing`), precise.
- **Problems counted, not thrown:** a read past an earlier layer's standing lattice, a plant wider than every cap, a slot overflowing.
- **`placement-compare`:** reads back every slot, count and problem; for each set the CPU plants its layers over the box round its held cells once and splits it by cell (`plantingByCell`); each cell's written layers compared by their bits in the field's layer (`comparePlantingCell`: matching, missing, extra, reordered).
- **Cost grows with:** each set's cells made a frame (its layers' lattices with borders; a set re-plants what it reads, the trees' coarse lattices cheap beside the understorey's 1.5 m one); memory with each set's slots times its written layers' lattices; candidates for the largest set's cells a frame.
- Tests: `planting_cells_test` (ring, slots, layout, cells planting the whole, sets' layers, each set planting as the field), `planting_records_test` (maps round trip, layers, jobs and a set's places, comparison, command, settings keys).

## Placed plants drawn (`gpu-placement` 2b2, 2c1, 2c2)

A field's `PlantingLooks` (on its `PlantingSource`, read every frame) names how each (layer, kind) is drawn: `ModelRenderer`s in the plant's own frame (shadow-only parts and sway included), drawn wherever planted. The cells run while looks are there, the setting off or not. The forest's trees, understorey and palms have no entities: these are all there is of them (`forest-trees.md`, `forest-plants.md`).

| Piece | What |
|---|---|
| Frame (`appendPlantedParts`) | each kind's parts as draws (`rendererPartDraw`, shared with entities), `plantedKinds` runs, the layers' drawing; attached by the engine as the frame's draws (bounds, levels, entries) |
| Table (`buildPlacedTable`) | parts by style (`GpuPlacedPart` 112 B: cull record and levels in the plant's frame, first group), a group a part's level slot (`GpuPlacedGroup` 32 B: its indices and its mesh's clusters), a kind's parts by index (`GpuPlacedKind` 64 B, `PLACED_MOST_PARTS` 12); a ground-following part's culling sphere grown by its layer's `groundRelief` (its vertices lifted onto the land: [terrain.md](terrain.md)); a kind given several looks (`PlantedKindLook::variant`, 0 to n - 1) draws the one its plant's pick chooses (`placedKindSlot`: min(pick x n, n - 1), mirrored), its variants past the first in its layer's top kind slots, refused where they reach a look's kind; each style's groups one run; each layer's reach (`PlacedLayerReach`: its parts' bounds across, up and down a unit of scale, seen, casting, most parts); refuses glass, unpooled meshes, past the passes' limits |
| Blocks (`placedBlocks`, `placedBlockRuns`) | over every set's slots (`PlantingSetSlots`: held, layout, field's layers, places, cell size), 64 places of a held slot's drawn written layer each (`GpuPlacedBlock`: its set's plant and count places, the field layer's kinds); a run a held cell's drawn layer, naming its set |
| Cell boxes (`placedCellChunk`) | a run's box as a `GpuCullChunk`: the cell (its set's size) grown by the reach at the layer's largest scale, the ground's range under it (`terrainHeightRange`, taken once while held) less the foot sink, the chunks' margin (`setCullChunkBox`); seen and casting flags from the layer |
| Views (`placedViews`, `GpuPlacedView` 304 B) | the camera, each probe face captured, each shadow tile drawing its casters (`setPlacedViews`; not the mirror: flat faces only); each takes the blocks of the runs whose box `chunkInView` passes, as items (`GpuPlacedItem`: block, view), its entries after the view before's, room for two a part of its layer's most-parted kind a plant (one in a shadow tile: no fades) |
| Instances | each part's `GpuInstance` packed with the moving draws' (`SceneRecords::planted`, the frame's materials), in the table's order, scene binding 44; the slots' plants 43, every view's entries 45 |

Four passes [PLACED], after the planting's and before the camera's mesh clusters (`VulkanPlacedDrawing`, `placed_cull_*.comp`), every view at once:

1. `count`: a workgroup an item (rows of `PLACED_ITEM_ROW`), a thread a plant in its count; each of its kind's parts placed (`placedCullRecord`) and flagged at every level slot with a mesh by the view's own `cameraCandidateFlags`, kept where the flags say drawn; the item's drawn per group counted.
2. `scan`: a workgroup a (group, view), the view's items' counts to each item's first place in the group.
3. `commands`: a workgroup a view, its groups' totals to their first entries (from its `entryBase`) and its groups' commands (`firstInstance` from `PLACED_INSTANCE_BASE`).
4. `write`: as `count`, each drawn part's entry (the plant with the draw list's bits, the part) at group base + item place + the item's earlier plants in that group: no atomics, so `cullPlacedOnCpu`'s order.

- **Drawing:** each pass draws its view's groups' commands, empty or not, a multi-draw a style (`drawPlacedDraws`): prepass and main (the camera; where it draws by mesh shaders, its placed entries are mesh clusters instead: `mesh-clusters.md`), probe faces, shadow tiles (with their casters; a tile keeping movers apart only into its still copy). `triangle_vertex.glsl` (prepass and main), `shadow.vert` and `shadow_cut_out.vert` read a placed entry for a draw from `PLACED_INSTANCE_BASE` (`drawnObject`, `drawn_objects.glsl`) and build the instance (`placedInstance`: the plant's matrix after the part's, its turn on the normals, the level sphere placed), the branch the same for a whole draw; the main pass's motion vectors take last frame's matrix as this one's (placed plants never move: only the sway does).
- **Shadow tiles redrawn** (`shadowTileNeeds`, `PlantedShadowChanges`): a tile taking a cell made this frame (each set's own list; cells keyed by set and cell) or dropped since the last, by a casting sphere round its boxes (`placedChangeRecord`); every tile when the table's signature (`placedTableSignature`: parts, kinds, groups) or the field changes, or placed plants start or stop.
- **The entity's matrix:** `placedPlantModel` is `Transform::getMatrix` of the plant's pose step for step (equal on the CPU); the GPU's sine and cosine differ in the last bits. `placedInstance`, run every vertex, makes the turn once and the matrix once (`placedTurnedModel`, `placedSphereBy`); `placedCullRecord` the matrix once for both spheres.
- **`placement-compare`:** also reads back a frame's entries, commands and slots and reports each view's groups' entries against `cullPlacedOnCpu` over those slots (`comparePlacedDraws`: matching, missing, extra, near an edge, reordered, commands), a line a view.
- **Cost grows with:** the CPU's cell tests with the held cells' drawn layers times the views (a box test each); the passes with each view's items (the blocks of its cells in view: a thread a plant, twice) and its groups (a scan each); memory with the items times the groups (counts) and the views' entries' room; shadow redraws with the cells made or dropped a frame.
- Tests: `placed_drawing_test` (the matrix equal to the entity's, parts' records and instances, table and layer reach, variants by pick, blocks and runs, two sets' blocks, cell boxes holding every part, views' layout, culling by boxes dropping nothing in camera and sun tiles, change records and signature, the lists equal to the entities' through `batchByMesh`, order, comparison, the frame's looks); `shadow_culling_test` (planted changes redraw their tiles); `terrain_test` (a box's height range); `forest_trees_test` (the forest's looks, stand-ins and sway, no tree entity); `understorey_test`, `palms_test` (their looks); `placed_drawing_gpu_test` (`make gputest`, `placedKindSlot` among them).
