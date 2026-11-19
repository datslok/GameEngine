# Planting from maps

Covers: `src/scene/planting`, `src/scene/planting_maps`, `src/scene/planting_shared.h`, `assets/shaders/planting.glsl`, `assets/shaders/hash64.glsl`, `src/core/hash_shared.h`, `src/scene/planting_cells`, `src/render/gpu/planting_records`, `src/render/gpu/planting_records_shared.h`, `src/render/vulkan/vulkan_planting`, `assets/shaders/planting_cells.glsl`, `assets/shaders/planting_candidates.comp`, `assets/shaders/planting_spacing.comp`, `assets/shaders/planting_write.comp`

Plants placed from maps by an ordered dither, a box at a time, the same on the CPU and the GPU (Horizon Zero Dawn's placement; research and the user's choices: `docs/research/VALLEY.md`). A game builds the maps and turns its settings into layer records; the forest's: `forest-plants.md`. Stage 2b1: the GPU plants cells round the camera and `placement-compare` checks them; nothing draws them yet (2b2).

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
| `PlantingCellSettings` (F5 Render > Planting) | off by default while nothing draws; 64 m cells, those within 300 m held, 4 made a frame, dropped a cell past the radius |
| `PlantingCellRing` | cells held and their slots: drops first, then the cells within the radius not held, nearest first (then row, column), each into the lowest free slot; a new size or radius starts again; ordered by cell, never address |
| `plantingSlotCount` | the cells touching twice the drop distance a side: 13 x 13 = 169 |
| `PlantingSlotLayout` | a slot: each layer's lattice over a cell (`plantingLatticeCapacity`); a cell made: each layer's lattice over its candidates' box |
| Records (`planting_records`) | maps header 80 B + words (bytes and halves packed), `GpuPlantingLayer` 432 B (keep-aways' reaches and spacing rings from the CPU), `GpuPlantingJob` 80 B a layer of a cell made (its three lattices, box, places), candidate 48 B, `GpuPlacedPlant` 32 B |
| `VulkanPlanting` [PLANTING] | uploads the field when it, its version, the cell size, radius or cells a frame change (every cell made anew); after WIND |

Three passes a layer over the frame's cells (a workgroup row a cell), in layer order, a barrier between each:

1. `planting_candidates`: every cell of the candidates' lattice written (`plantingCandidateAt`, `candidateAt` line for line), kept cleared; earlier layers' kept plants read through their job of the same cell.
2. `planting_spacing`: over the standing lattice, kept unless it gives way; reads only neighbours' candidate marks.
3. `planting_write` (after every layer): a workgroup a cell writes the kept plants in its box in rows from the north by a 256-wide prefix sum, no atomics, and the count.

- **Same plants, same order as `plantLayers`:** the map reads (`planting_cells.glsl`) mirror `planting_maps.cpp`; the ground is `terrainSurfaceHeight` in `terrain_heights.glsl`, by the grid's reciprocal spacing from the CPU (`GpuTerrainGrid::inverseSpacing`), precise.
- **Problems counted, not thrown:** a read past an earlier layer's standing lattice, a plant wider than every cap, a slot overflowing.
- **`placement-compare`:** reads back every slot, count and problem; the CPU plants the box round the held cells once and splits it by cell (`plantingByCell`); each cell's plants compared by their bits (`comparePlantingCell`: matching, missing, extra, reordered).
- **Cost grows with:** the cells made a frame (their layers' lattices with borders); memory with the slots: the forest's 3,098 plants a slot (2,209 of them the understorey's 1.5 m lattice), 16.8 MB for 169 slots; candidates for 4 cells.
- Tests: `planting_cells_test` (ring, slots, layout, cells planting the whole), `planting_records_test` (maps round trip, layers, jobs, comparison, command).
