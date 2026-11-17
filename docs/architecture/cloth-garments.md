# Cloth garments

Covers: `src/cloth/cloth_pattern`, `src/cloth/cloth_seams`, `src/cloth/cloth_self_collision`, `src/cloth/cloth_yardstick`, `src/math/delaunay`, `src/app/cloth_yardstick_run`

Garments cut from flat pieces and sewn, cloth kept off itself, and the yardstick every solver is judged by. The step and settings: `cloth.md`; tethers measured along pieces: `cloth-solvers.md`. Research: `docs/CLOTHGPU.md` (Part 3 self-collision, Part 5 the yardstick and its tables). The cloth lab's dress and mannequin: `games/clothlab/cloth_lab_dress`.

## Patterns

`cloth_pattern`, as a dressmaker's paper pattern draws them.

- `PatternPiece`: a closed outline in metres of `PatternEdge`s (polylines, each starting where the last ends), counterclockwise seen from the outside (+z).
- `PatternSeam {firstPiece, firstEdge, secondPiece, secondEdge}`; `Pattern {pieces, seams}`.
- Two counterclockwise outlines run opposite ways along a shared edge once sewn, so a seam joins point k of the first edge to point n - k of the second.
- `edgeLength`, `resampleEdge` (evenly spaced, ends exact).

**Cutting** (`meshPattern(pattern, spacing)` -> `PatternMesh`):

1. Each edge gets the fewest segments keeping points at most `spacing` apart; both edges of a seam take the longer one's count.
2. A triangular lattice fills each piece (fixed in the pattern's plane), points kept only at least `latticeClearance` (0.6) spacing from the outline, which keeps border triangles from going thin.
3. `delaunayTriangulation` of all the points; each outline segment must be one of its edges (`std::logic_error` for an outline too sharp for the spacing).
4. Triangles whose middles lie outside are dropped.

- Why equilateral: the same every way, where a grid's one diagonal a square is stiffer along it.
- `PatternMesh`: `mesh` (pieces side by side in z = 0, UVs their flat positions in metres), `flatPositions`, `pieces`, `edgePoints[piece][edge]`, `seams` (each seam's stitches in order).
- Throws `std::invalid_argument` for a bad spacing, an open or clockwise piece, a seam on a missing or already-sewn edge, or a point in two stitches.
- `wrapOnCylinder(flat, axisFoot, radius, facing)`: lays a piece round an upright cylinder, lengths kept, outside outwards, as garment software lays panels round a body before sewing.

## Delaunay

`math/delaunay`, `delaunayTriangulation(points)`: Bowyer-Watson.

- Points inserted along a Hilbert curve over their box (`hilbertIndex`), so locating each walks a few steps from the last triangle made.
- The triangles whose circumcircles hold the point form a connected cavity, fanned from the point.
- Doubles, scaled into a unit square, inside a far triangle a thousand times out, dropped at the end.
- Lattices put four points on a circle: ties count as outside, a triangle the point lies on the side of joins the cavity, and `trimCavity` trims until the point sees every cavity side from inside. Rounding can then only choose between equally good triangles, never overlap them.

## Stitches

`cloth_seams`, `ClothStitch {first, second}` in `Cloth::stitches`; each point in one stitch at most, so stitches solve in any order or all at once.

- `solveStitch`: a weld closing at `seamSpeed`. After each substep the gap may be at most `stitchWidest` = gap at the substep's start - closingStep (seam speed times the substep). A wider gap is pulled in, shared by inverse mass; once nothing is allowed, both points land on their mass-weighted meeting point exactly. Mirrored by `cloth.glsl`'s `clothStitch`.
- Measured from the substep's start, not from where the points are: the pull becomes velocity, so a pull from the current place adds speed every substep and slams seams shut.
- Shut, it is rigid.
- Placement in the substep: after the tethers, before the bodies.
- **Sliding while sewn:** `markSewing` marks points of stitches still open; `solveContacts` pushes those out without friction. A stitch's pull along a chord runs into the body round a shoulder or armpit, and static friction would otherwise freeze the seam open. Once shut, normal friction.
- `seamHinges`: for each two stitches in a row, a bending hinge on the first piece's segment between them, wings the far corners of the triangles on each side, rest shape the second triangle unfolded flat beside the first (`restHinge`). A sewn seam bends like the fabric round it instead of folding freely. They join the cloth's bending and its colouring.

**Sewn into a cloth:** `clothSewing(meshed)` -> `ClothSewing {stitches, hinges, pieces}`; `makeCloth(rest, start, settings, sewing)` takes the flat pattern as the rest shape (lengths, masses, bending, rest positions) and starts the points still at `start` (pieces wrapped round a body).

## Self-collision

`cloth_self_collision`, CPU only; `selfCollision` off by default and the GPU ignores it. Layers of one cloth stay `thickness` apart and never pass through each other; separate cloths do not collide with each other.

**Who never meets** (`ClothSelfTopology`, `Cloth::selfTopology`, from `clothSelfTopology` at `makeCloth`):

- `joined`: each point's representative (the lower point of its stitch), so stitched points count as one.
- `neighbourStarts`, `neighbours`: representatives one edge apart (`nearInCloth`); `edges` once each; `restPositions`.
- Features sharing a representative never meet. Neighbours are ordinary contacts unless closer than the thickness at rest: skipping all neighbours let a coarse strip fold a square through the next.

**Pairs** (`ClothSelfPair {kind, points, normal, gap, heights}`, by `pointTrianglePair` or `edgeEdgePair`): as found, the normal from the second side's nearest point to the first's (the triangle's normal or the edges' cross product where they meet), the gap, and each point's height above the second side along the normal. Nearest points by `closestTriangleFeature` (barycentric `weights`) and `closestPointsBetweenSegments` (`firstShare`, `secondShare`), in `math/closest_points`.

**Finding** (`findClothPairs(positions, triangles, topology, reach, thickness, jobs)`):

- A spatial hash by counting sort (two passes over boxes' cells, about two buckets a cell entry, Teschner's primes; an item over 16 cells an axis goes on a list every query reads). Cells the mean edge plus two reaches wide.
- Points against triangle boxes grown by the reach, then edges against edge boxes; 256 queries a job, lists joined in order, so any thread count gives the same pairs in the same order.
- Each query marks what it met in a per-thread list (mailboxing) instead of sorting. Candidates checked cheapest first: box, shared representative, squared distance, then the neighbours' rest gap.

**In the substep:** pairs found every `selfCollisionInterval`-th substep from the points then (kept as the cut's start); then the solver, tethers, stitches, bodies, then `solveSelfContacts` and `cutClothMotion`, last. Self-contacts after the bodies: before them, the floor gave back the bottom layer's share of every push and the gap shrank window by window.

**Contacts** (`solveSelfContacts`, on one thread, in order):

- Each pair closer than the thickness, measured between its nearest points now, is pushed apart to it (OGC's offset geometry). C = distance - thickness >= 0, coefficients +weight on the first side and -weight on the second; lambda = (thickness - distance) / sum(coefficient^2 / mass). The gap opens by exactly what was missing; no centre of mass moves.
- Then friction on the slide between the nearest points, as against bodies.

**The cut** (`cutClothMotion`, Planar-DAT, Chen, Hsu, Ayman & Macklin 2026): every free point's motion since the pairs were found is cut short of each pair's dividing plane.

- `dividingHeight(pair, motions)`: room shared in proportion to each side's own approach (the first side keeps gap delta_1 / (delta_1 + delta_2), half when neither approaches). Follows the paper's text; its printed lambda would give a point falling onto a still triangle no room.
- `cutShare(distance, approach)` (Eq. 16): the whole motion unless it reaches the plane before 1 / gamma of it, else gamma of the way (`clothCutRelaxation`, `CLOTH_CUT_RELAXATION`, 0.85). A point starting on or past its plane (rounding only) stops.
- Each point takes its least share, then a cap of 0.5 gamma reach on its motion, for anything beyond the reach the search did not see. A point kept whole stays exactly where it is; pinned points are never cut.
- **Speed limit:** about 0.43 reach per interval (3.8 m/s at the defaults). The reach is 1.5 thicknesses so that a 1 cm mesh's flat neighbours are not pairs.
- `firstSidePoints(kind)`: how many of a pair's points are on the first side.

**Test oracle:** `countClothCrossings`, every edge against every triangle sharing no representative, through the same hash. Slow; tests only.

**Cost:** finding pairs dominates (dress-sized cloth is several times slower with it on); the CPU path is for tests and small cloths, cloth that size belongs on the GPU. Numbers: `docs/BUILDLOG.md`.

## The yardstick

`cloth_yardstick`: how every solver is judged. Tables: `docs/CLOTHGPU.md`, Part 5. Commands: `src/cloth/CLAUDE.md`.

- **Why it measures the solver:** a fixed compliance per edge is the same fabric at any square resolution, so stretch past the fabric's own, or any difference between rungs, is solver error.
- `yardstickSheet(squares, settings)`: a 1 m sheet (`yardstickSheetSize`), upright in x-y, top edge on y = 0, pinned at its top corners. `yardstickRungs`: 25, 50, 100, 200 squares a side.
- `measureSheet` -> `SheetMeasure`: `pathStretch` (the longest top-to-hem column against its rest length, minus 1), `worstEdgeStrain`, `sag` (hem middle below the pins; over 1 m is stretch).
- `settleYardstick(cloth, seconds, tickSeconds, jobs, fields, prepare)`: steps against field bodies, each tick readied by `prepare` (`ClothTickPreparation`, given the cloth and tick number); returns the farthest any point moved over the last second.
- `measureGarment(cloth, hem, floorHeight, fields)` -> `GarmentMeasure`, for cloth without columns: `meanStretch` (squashed edges count as none), `percentileStrain` (`garmentStrainPercentile`, 99%: the stretch carrying weight, without the odd edge pinched at a seam), `worstEdgeStrain`, `worstSeamGap`, `hemHeight`, `closestToBody` (infinity with no bodies).

**The runner** (`app/cloth_yardstick_run`; options parsed in `app/game_registry`; each takes no other option):

| Option | Runs |
|---|---|
| `--cloth-yardstick` | Every rung settled `yardstickSettleSeconds` (10 s): CPU XPBD, XPBD + tethers, vertex block descent, and it with every level; GPU the same four; at the fabric's substeps and four times as many (vertex block descent at the fabric's only). Then the dress test |
| `--cloth-yardstick-quick` | 25, 50, 100 squares and the dress at 2, 1.4, 1 cm, 4 s, CPU only, fabric's substeps. For development; not recorded |
| `--cloth-yardstick-dress` | The dress test alone |

- **The dress test:** the cloth lab's dress on its mannequin (`mannequinField`, `dressOnMannequin`, `prepareDressTick` each tick) at `dressRungs` 2, 1.4, 1 and 0.7 cm, held by contact alone and pinned at the shoulders with tethers, XPBD on CPU and GPU (each GPU tick's commands taken after the tick's preparation, as `stepCloths` takes them; the mannequin's field in one `VulkanClothFields`). A row each: `GarmentMeasure`, last second's motion, time a tick.
- Each row's time a tick excludes making the sheet, its colouring and its levels.
