# Cloth

Covers: `src/cloth/cloth`, `src/cloth/cloth_particles`, `src/cloth/cloth_constraints`, `src/cloth/cloth_collision`, `src/cloth/cloth_grab`, `src/cloth/cloth_shared.h`

**Parked.** Rules, test status and measuring commands: `src/cloth/CLAUDE.md`. Research and every recorded number: `docs/research/CLOTHGPU.md`.

The cloth module sits above `physics/` and `scene/`, below `render/`, `engine/` and the games. This file: points, the XPBD step, constraints and colouring, pins, bodies, the hand, settings, threads and determinism. Elsewhere:

| Topic | File |
|---|---|
| The substep's energy, vertex block descent, coarse levels, tethers | `cloth-solvers.md` |
| Patterns, stitches, self-collision, the yardstick | `cloth-garments.md` |
| The GPU port and the engine side | `cloth-gpu.md` |

## Files

| File | Holds |
|---|---|
| `cloth/cloth_particles` | `ClothParticles`, prediction and the step's end |
| `cloth/cloth_constraints` | Stretch and bending constraints, their colouring and solves |
| `cloth/cloth_collision` | Contacts with shapes and distance fields, friction |
| `cloth/cloth_grab` | The hand, picking by ray |
| `cloth/cloth` | `ClothSettings`, the `Cloth` component, pins, `stepCloth`, `stepCloths` |
| `cloth/cloth_shared.h` | Thresholds shared with `cloth.glsl` (squashed edges and hinges, singular blocks, tether slots, the cut's relaxation) |

## The points

`ClothParticles`: structure of arrays, point i = mesh vertex i: `positions`, `previousPositions` (at the step's start), `velocities`, `inverseMasses`.

- Inverse mass 0 = pinned: nothing moves it, with no special case anywhere.
- `makeClothParticles(mesh, areaDensity)`: lumped masses, a third of each triangle's area times kg per m2. Throws for a bad density or a point in no triangle with area.
- `pinParticle`: inverse mass 0, still.

## One substep

`stepCloth(cloth, seconds, colliders, jobs, fields)`: `settings.substeps` substeps, each in this order:

1. Self-collision pairs found, every `selfCollisionInterval`-th substep (`cloth-garments.md`).
2. `predictCloth`: symplectic Euler, velocity first, then position by the new velocity; the start kept.
3. Moving pins slid (`slidPinPosition`).
4. The solver: XPBD (`solveStretchColours`, `solveBendColours` on substeps `bendingSubsteps` names, `solveGrab`), or vertex block descent's sweep with optional coarse levels (`cloth-solvers.md`).
5. `solveTethers`, then `markSewing` and `solveStitches`.
6. `solveContacts` against the bodies near the cloth: after the cloth's own constraints, so nothing is left inside a body.
7. `solveSelfContacts`, then `cutClothMotion`, last.
8. `finishClothStep`: velocity = motion / step; then drag on free points (each velocity times e^(-drag h)).

- Why symplectic Euler: `finishClothStep` then gives back exactly the predicted velocity, so a free sheet falls at g. Moving by v t + a t^2 / 2 would give back v + a t / 2.
- `stepClothParticles`: stretch, a hand and bodies alone, for tests. Both throw for fewer than one substep.

## Stretch (XPBD)

Macklin, Müller & Chentanez 2016.

- `StretchConstraint {first, second, restLength}`: C = |x_first - x_second| - restLength.
- `stretchConstraints(mesh)`: every triangle edge once (first < second), sorted, at its mesh length. A grid's diagonals also resist shear.
- `solveStretch`: one Gauss-Seidel pass, multiplier from 0 each substep: delta lambda = -C / (w1 + w2 + compliance / h^2). Skips edges pinned at both ends or shorter than `CLOTH_SHORTEST_EDGE`. Centre of mass stays put.
- Compliance (m/N) = 1 / stiffness; 0 holds lengths exactly; a positive value is the same spring whatever the step.

## Bending

`BendConstraint {wing0, wing1, edgeStart, edgeEnd, restAngle, stiffnessShare, isometricWeights}`: a hinge, two triangles sharing an edge.

- `bendConstraints(mesh)`: every edge shared by exactly two triangles, sorted, at its mesh angle. `stiffnessShare` = 3 l^2 / A (discrete shells, Grinspun et al. 2003).
- `restHinge`: a hinge from any four points at rest (used for seams).
- `BendingModel` (`ClothSettings::bendingModel`, live):

| Model | C | Rests | Notes |
|---|---|---|---|
| `Isometric` (default) | sum of K_i x_i, a vector | Always flat | Bergou et al. 2006. Fixed weights from `isometricBendWeights` (sum zero); gradient is K_i times identity, so no normals, angles or roots: a few times cheaper. Softer than the angle in deep folds |
| `Hinge` | angle - restAngle, wrapped | At the mesh's angles (pressed creases) | `hingeAngle` by atan2 of sine and cosine (acos has infinite slope at flat); `hingeAngleGradient` Bridson et al. 2003's closed form, sign measured from normal1 towards normal0 |

- `solveBend`: one pass, alpha = 1 / (B stiffnessShare). The model is chosen once a pass (`withBendStep`), not per hinge. B = 0 skips bending.
- Units: B is bending rigidity in N m, as the cantilever test measures it. Fabric values: the `ClothSettings` comment.
- Convergence: one pass carries stiffness about a hinge, so a finer mesh needs more substeps for stiff fabric; at real fabrics' B meshes agree well.

**Bending less often** (`bendingInterval`, XPBD only): `bendingSubsteps(step, substeps, interval)` says how many substeps a solve stands for (interval at every interval-th, the remainder on the tick's last, 0 elsewhere). Each solve divides alpha / h^2 by that count (`substepsCovered`), so a soft constraint gets the same impulse: multiple time stepping. Stays 1 by default: stability depends on spacing, fabric and step together.

## Colouring and threads

- `colourStretch`, `colourBend`: each constraint gets a colour none of its points has (at most 64, else `std::length_error`), DSatur order (most distinct neighbour colours first, ties to most neighbours, then the first). Each list is sorted by colour; `Cloth::stretchColours` / `bendColours` give where each starts.
- `solveStretchColours`, `solveBendColours`: colours in turn, each colour's constraints on every core. No two in a colour share a point, so any thread count gives the same bits; the colouring is the one-thread order too.
- Job sizes (`cloth_constraints.cpp`): `stretchPerJob` and `isometricBendPerJob` 2048, `hingeBendPerJob` 256. Small pieces of cheap work bounce points' cache lines between cores.
- Contacts stay on the calling thread: a point's test is a few nanoseconds.
- `stepCloths` runs a job per cloth, gathered in the world's order; each writes only its own points.
- The limit is the barrier after each colour; the job system's workers spin before sleeping for that reason (`engine-core.md`).

## Moving pins

- `ClothPin {particle, position}` in `Cloth::pins`; `addClothPin` (one), `addClothPins` (several, rebuilding once). Both call `pinClothPoint`, which pins for good and rebuilds the coarse levels and tethers.
- The game sets each `position` from its commands before a tick. Each substep slides the pin a share of the way, exactly there on the last: no jolt, its velocity is its motion. Drag leaves pins alone.

## Bodies

`cloth_collision`:

- `surfacePoint(shape, point)` -> `SurfacePoint {distance, normal}`: signed distance (negative inside) and outward normal for spheres, capsules and boxes. Inside a box: through the nearest face (ties to -x, +x, -y, +y, -z, +z in that order). At a centre: straight up.
- `surfacePoint(field, point)` / `fieldSurface`: a `DistanceFieldCollider`'s trilinear blend and slope as a unit normal (`physics/signed_distance_field`); straight up where the slope vanishes.
- `solveContacts(particles, shapes, settings, fields, sewing)`: shapes, then fields. A free point closer than `thickness` is pushed out along the normal, then positional Coulomb friction (Macklin et al. 2014) on its slide this substep: undone below `staticFriction` times the push, else shortened by `kineticFriction` times it. Points marked as being sewn get no friction (`cloth-garments.md`). Pinned points never move.
- Only points collide: a body smaller than the spacing can poke between them.
- `worldColliderShapes` (every `Collider` by `worldShape`), `worldDistanceFields` (`ClothFields`, each field shared): gathered once a tick; each substep keeps those whose boxes meet the cloth's grown by the thickness.
- Bodies are still and infinitely heavy; their motion is not in the friction. No restitution: friction on positions holds a resting sheet exactly still, where a velocity rule would let it creep.

## The hand

- `ClothGrab {particle, target}` in `Cloth::grab`, set by the game from its commands before a tick.
- `solveGrab`: XPBD with C = |x - target| and `grabCompliance`: a stiff spring, so a yank cannot over-stretch the edges round the point. A pinned point cannot be dragged.
- Let go, the point keeps the velocity its motion gave it.
- `raycastCloth(cloth, ray, maxDistance)` -> `ClothHit`: `raycastTriangle` against `Cloth::triangles` as they are now; the hit triangle's corner nearest the hit.

## Settings

`ClothSettings` (`cloth/cloth.h`, `visit` beside it, F5's Cloth group): every field, unit and default with its reason is in the header and its comment. The ones that shape the design:

| Setting | Default | Why it matters |
|---|---|---|
| `stretchCompliance` | 0.0005 m/N | 1 / (E t); per edge, so the same fabric at any square mesh resolution |
| `substeps` | 30 | Small Steps (Macklin et al. 2019): substeps beat passes for the same work |
| `bendingModel`, `bendingInterval` | `Isometric`, 1 | Above |
| `method`, `hierarchyLevels` | `Xpbd`, 0 | `cloth-solvers.md`; both read each tick, on CPU and GPU |
| `tethers`, `tetherSlack` | on, 0.002 | `cloth-solvers.md` |
| `seamSpeed` | 1 m/s | `cloth-garments.md` |
| `selfCollision` | off | CPU only; the GPU ignores it |
| `thickness` | 5 mm | Gap kept from bodies and between layers, so cloth rests instead of flickering half inside |

## The component

- `Cloth` (`cloth/cloth.h`): settings, points, coloured constraints, pins, stitches and pieces, triangles and `selfTopology`, `restPositions`, `hierarchy`, `tethers`, `pointConstraints`, `vertexColours`, `grab`, and the GPU's `onGpu` and `gpuTicks`. Simulated in world space; its `Transform` is unused.
- `makeCloth(mesh, placement, settings)`: the mesh placed first, so masses, lengths and angles come from the placed sheet. `makeCloth(rest, start, settings, sewing)`: rest shape from a flat pattern, points starting at `start` (`cloth-garments.md`).
- `stepCloths(world, seconds, jobs, solver)`: every cloth against the world's colliders and fields. On the CPU, then each cloth's points copied into its entity's `DeformingMesh` (drawn from a mesh at the identity), so simulation never depends on drawing. With `ClothSolver::Gpu`, queues `clothTickCommands` instead (`cloth-gpu.md`).

## Determinism

- Same commands, same bits on any thread count: colours, per-cloth jobs, and per-point jobs each write only their own data.
- `hashSimulationState` (`engine/state_hash`) hashes each CPU cloth's positions, velocities and inverse masses, and every cloth's pins and hand. A GPU cloth's points are left out (repeatable on one GPU and driver only).

## Float precision

- Each substep gravity moves a point g h^2 (under a micrometre at 30 substeps), near a float's resolution a metre from the origin, so rest balances settle slightly off and momentum drifts a little more at higher substep counts: one reason not to go past 30.
- Invisible at cotton's stretch. Cloth far from the origin would want its points relative to an origin of its own.
