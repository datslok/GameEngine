# Cloth solvers

Covers: `src/cloth/cloth_energy`, `src/cloth/cloth_block_descent`, `src/cloth/cloth_hierarchy`, `src/cloth/cloth_tethers`

What carries a pin's pull across a fine sheet, which a local solver (XPBD or vertex block descent alone) moves about one edge a pass, so fine sheets hang stretched. Two answers live here: coarse levels under vertex block descent, and tethers under either solver. The step, XPBD and the settings: `cloth.md`. Papers, formulas and the yardstick's numbers: `docs/CLOTHGPU.md`, Parts 2 and 5.

| | Coarse levels | Tethers |
|---|---|---|
| Solver | Vertex block descent only | Either |
| Helps | Any held cloth, pinned or resting on bodies | Cloth hanging from pins only |
| Cost | Many times XPBD's (each level's gradient and line search over the whole cloth) | About XPBD's |
| Default | Off (`method` XPBD, `hierarchyLevels` 0) | On |

## The substep's energy

`cloth_energy`. A substep is an implicit Euler step: positions minimising

phi(x) = sum m |x - prediction|^2 / (2 h^2) + sum over edges C^2 / (2 compliance) + sum over hinges B stiffnessShare |C|^2 / 2 + the hand's spring.

- Vertex block descent and the levels both lower this one phi, so they never fight. XPBD solves the same problem through multipliers; a move it did not make breaks their bookkeeping, which is why XPBD cannot take the levels.
- `ClothEnergyTerms`: references to the constraints, `ClothPointConstraints` (each point's edges and hinge corners as compressed rows; `clothPointConstraints`, `Cloth::pointConstraints`), compliances, bending model, hand. A compliance of 0 becomes `stiffestClothCompliance`, since a perfectly stiff edge's energy is infinite. `energyStiffness` turns a compliance into N/m.
- `clothStepEnergy`: phi in double, for tests.
- `clothPointForce(point, ..., block)`: minus phi's gradient at one point, optionally with its 3 x 3 second-derivative block (`ClothStiffnessBlock`, `solve` for the Newton step).
- It is a sum of terms in a fixed order, each added to the force and block so far: `addInertiaTerm`, `addEdgeTerm` per edge (stiffness across the edge only while stretched, so the block stays positive), `addIsometricHingeTerm` or `addHingeAngleTerm` (Gauss-Newton) per hinge corner, `addHandTerm`. `cloth.glsl` mirrors each term, to the bit on the GPU's sum order (`cloth-gpu.md`).
- `clothEnergyGradient`: every point's gradient in one pass, zero at held points.
- `clothLineSearch`: along a direction, the minimum of phi's quadratic model, step = -slope / curvature (`lineSearchStep`), the curvature from `inertiaCurvature`, `edgeCurvature`, `isometricHingeCurvature`, `hingeAngleCurvature`, `handCurvature`. Summed in chunks added in a fixed order; nothing unless downhill.

## Vertex block descent

`cloth_block_descent`, Chen, Liu, Yang & Yuksel 2024. `ClothSettings::method` = `VertexBlockDescent`.

- Each free point takes the exact Newton step for its own position with the others held: `clothPointForce` divided by its block.
- `clothVertexColours` -> `ClothVertexColours {order, colours}`: each point the lowest colour none of its edge or hinge neighbours has, in index order (a grid needs 4; at most 64). `Cloth::vertexColours`.
- `solveVertexBlocks`: colour by colour, each on every core; same bits on any thread count. Pinned points stay.
- Positions only, no multipliers: a coarse level's move is simply where the next sweep starts.
- Alone it stretches about as XPBD does (a local solver too); it exists for the levels.
- Bends every substep inside each point's step (`bendingInterval` does not apply).

## Coarse levels

`cloth_hierarchy`. Müller 2008's levels, each a Galerkin coarse step on phi (Lu, Yuan, Mo & Hu 2025).

**Structure** (`ClothHierarchy`, `Cloth::hierarchy`):

- `levels[0]` is level 1, the first coarser than the cloth (level 0).
- Each `ClothLevel`: its `points` (a sorted subset of the level below's), coarse mesh (`neighbourStarts`, `neighbours`), `children` (points below not on it), P's rows straight from the cloth's points (`spreadStarts`, `spread`) and columns (`gatherStarts`, `gather`), and `masses` (diagonal of P^T M P: each region's weight to its motion).
- `pointLevels`: each point's coarsest level. `parentStarts`, `parents` (`ClothLink {point, weight}`): each child's parents on the level above.

**Building** (`buildClothHierarchy`, from `restPositions`, the edges and the pins):

- `chooseCoarsePoints`: the paper's greedy sweep in index order. A point goes fine if it keeps at least `clothParentsWanted` (3) coarse neighbours and every fine neighbour keeps more; pinned points never go fine. The paper's 2 made deep hierarchies unstable.
- `parentWeights`: 1 / (d / farthest d + 0.001), normalised.
- A fine point collapses into its parent nearest the parents' mean; its other edges move to that parent.
- Stops when a level would not shrink, would hold fewer than `smallestClothLevel` (50) points, or at `maxClothLevels` (12). Small cloths get none.
- P's rows follow the parents up at build time. Rebuilt by `pinClothPoint`.

**The coarse step** (`solveClothHierarchy`), from the coarsest level used down to level 1:

1. Held points (pinned, or within the thickness of a body) are truncated out.
2. phi's gradient, zero at held points.
3. Each level point gathers its column and steps by minus it, divided by its region's mass / h^2 plus its coarse edges as springs of the fabric's compliance (`addCoarseEdgeTerm`; only shapes the direction).
4. Spread down by P, held points staying.
5. Line search along the result.

- At phi's minimum the gradient is zero and nothing moves; each step lowers phi to float rounding.
- A substep with levels: sweep, coarse levels, sweep (smoothing on both sides, as multigrid does: spreading leaves fine wrinkles).
- `restrictToLevel`, `prolongFromLevel`: P^T and P alone, for tests.
- `ClothSettings::hierarchyLevels`: levels a tick uses (all it has if fewer).

## Tethers

`cloth_tethers`, long-range attachments (Kim, Chentanez & Müller 2012): an invisible string from each free point to each of its nearest pinned points.

- C = |x - anchor| - restLength (1 + slack) <= 0, active only when taut: never stops folding, bunching or swinging round the pins, only stretching away from them.
- `solveTether`: the anchor is pinned, so XPBD's step with no compliance puts the point back on its sphere along the line from the anchor. Mirrored by `cloth.glsl`'s `clothTether`.
- `solveTethers`: each point's tethers nearest first; they touch only that point, so points are shared among cores (1,024 a job) with the one-thread bits.
- `ClothTether {anchor, restLength}`, `Cloth::tethers`: `tethersPerPoint` (`CLOTH_TETHERS_PER_POINT`, 4) slots a point, nearest first (ties in anchor order), spare slots and pinned points' slots `noTetherAnchor`. Empty with nothing pinned. Both corners of a yardstick sheet are needed, or its top edge could sag between them.
- `clothTethers(restPositions, inverseMasses, pieces, jobs)`: built once when pinned; `pinClothPoint` and `addClothPins` rebuild.
- Placement: after either solver (XPBD's hand included), before stitches and bodies, so bodies have the last word.
- `tetherSlack` (0.002 of the rest length): about cotton's own stretch, so strings take out only the solver's error.

**Rest length along the fabric** (`ClothRestPaths`): a tether shorter than the fabric between its ends would pull the fabric tight.

- `ClothPieces` (`Cloth::pieces`, from the sewing): each point's piece and each piece's outline, counterclockwise in the rest x-y plane.
- Within a piece: the shortest path inside its outline, which bends only at reflex corners. A visibility graph over those corners (`sees`: crosses no side properly and its middle is inside, so a chord across a neckline is blocked), Dijkstra once per anchor; a point's distance is the straight line if it sees the anchor, else the best corner it sees plus that corner's distance.
- Different pieces: never tethered (no tether crosses a seam).
- No pieces (a sheet): straight lines, exact for a flat piece without inward corners.
- Not the edge graph's shortest path: on a grid with one diagonal a square it runs long, and any excess is stretch the tether allows.
- Geodesics across seams on a worn garment are not built.
