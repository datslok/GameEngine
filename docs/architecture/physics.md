# Physics

Covers: `src/physics/`, `src/math/closest_points`, `src/scene/collision_mesh`

Shapes, queries, the broad phase, the static level, particles, signed distance fields and the kinematic character controller. Everything is plain C++ on `math/`, `ecs/` and `core/`: no GPU, no SDL. Movement tuning rule: `.claude/rules/character-tuning.md`. Who drives the controller (characters, first-person bodies): `gameplay.md`.

## Shapes and overlaps

- One shared space, usually the world. `Sphere` (centre, radius), `Capsule` (segment `a`-`b` grown by `radius`; `a == b` is a sphere), `Aabb` (`math/aabb`, never rotated; also mesh bounds via `meshBounds`, `Model::getBounds`).
- `Shape` = `std::variant<Sphere, Capsule, Aabb>`; `boundsOf(Shape)` is the box the broad phase files it under.
- `overlaps(a, b)` (`physics/overlap`): every pair, either order, plus `Shape` against `Shape` through `std::visit`. Touching counts, so a character standing exactly on a floor is in contact.
- Rounded shapes reduce to closest points between cores against the summed radii (`math/closest_points`):

| Function | Method |
|---|---|
| `closestPointOnSegment`, `closestPointInBox` | Clamp |
| `closestPointsBetweenSegments` | Ericson |
| `closestPointsSegmentBox` | Segment cut where it crosses the box planes; each quadratic piece minimised exactly |
| `closestPointOnTriangle` | Voronoi regions (Ericson 5.1.5); nearest edge for a degenerate triangle |
| `closestTriangleFeature` | The same point plus the face, edge or corner it lies on (distance field signs) |
| `closestPointsSegmentTriangle` | A piercing crossing, else the best of both ends against the triangle and the segment against each edge |

## Raycasts

`physics/raycast`: `raycast(ray, Sphere | Capsule | Aabb | Shape | TriangleMesh, maxDistance)` → `std::optional<RayHit>` (`distance` in world units even for a non-unit direction, `point`, unit `normal` always facing back along the ray). `maxDistance` defaults to `unlimitedRayDistance`.

- A ray starting inside a shape hits at distance 0, normal opposite the ray, so a query never sees through what it starts in. A zero direction hits nothing.
- Spheres and capsule ends: the ball quadratic. Capsule side: motion along the axis removed. Boxes: slabs, the latest entry axis gives the face.
- `raycastTriangle`: the one Moller-Trumbore test (both sides), shared by meshes and the level. Barycentrics may stray `edgeTolerance` (1e-5) outside 0..1, so a ray through a seam two triangles share cannot slip between them (a bit-exact regression test).
- `raycast(ray, mesh, model, maxDistance)`: the ray carried into the mesh's own coordinates by `affineInverse` (`math/mat4`, cofactors as `normalMatrix`; none for a flat matrix), distances scaled back by world length per local length along the ray, normal returned through `normalMatrix`.
- `transformPoint` and `transformDirection` (`math/mat4`) apply a matrix with w = 1 or 0.

## Triangle meshes for collision

- `TriangleMesh` (`physics/triangle_mesh`): `positions` + 3 `indices` per triangle (counterclockwise from the front), no normals or UVs, usually already in world space.
- `collisionMesh(mesh, transform)` (`scene/collision_mesh`) builds one from a render `Mesh`, checking indices; `appendCollisionMesh` adds to an existing one with shifted indices.
- `AssetManager::makeCollisionMesh(renderer)` merges every part of a `ModelRenderer`, each placed by its `localTransform`, so what is drawn and what is solid agree.

## Colliders

`Collider` (`physics/collider`): a `Shape` in the entity's own coordinates plus optional `detail`, a `std::shared_ptr<const TriangleMesh>` in the same coordinates, shared between entities with one model. The shape must enclose the detail, since its hit distance is a lower bound for the triangles'.

- `worldShape(shape, model)`: points move exactly; radii grow by the largest column length of the 3x3 part (exact for rotation times scale); a box becomes the axis-aligned box round the turned box (half-size `sum_j |A_ij| h_j`).
- `triangleMeshBounds`; `meshCollider(mesh)`: the mesh's box as shape, the mesh as detail.
- `raycastColliders(world, ray, detail, maxDistance)` (`physics/collider_raycast`): builds a `CollisionScene` and asks it once, at current (not interpolated) poses. For many queries keep a scene.
  - `RaycastDetail::Shapes`: one cheap test per entity. `Triangles`: a shape hit is confirmed against the detail mesh; a ray slipping between its triangles goes on to what is behind.
  - The nearest `ColliderHit{entity, hit}` wins and shrinks the reach.
- Debug: `DebugDraw::shape` draws any `Shape`; `drawColliders` (`engine/debug_views`, interpolated pose, `colliderDebugColour`) in the F4 view.

## Broad phase: `UniformGrid`

`physics/uniform_grid`, tuned by `CollisionSettings` (`physics/collision_settings`: `gridCellSize` 2, about a character; `largeItemCells` 64).

- Items are small dense numbers with a box; the caller keeps what they mean. `cellOf` uses floor, so -0.5 is in cell -1.
- Occupied cells only, in an `std::unordered_map` with Teschner's prime-xor hash, so the world can be any size.
- An item goes in every cell its box touches, or on the large list (checked by every query) past `largeItemCells`.
- `insert` throws for a non-finite box or one too far out for integer cells.
- `clear` keeps memory for the next fill and erases only cells left empty since the last clear.
- `query(region, found)`: items whose own box overlaps, each once.
- `walkRay(ray, reach, visit)` (template in the header): large items first, ray clipped to the occupied cells' box, then cells near to far by Amanatides and Woo (`tMax`, `tDelta`); `visit(item, reach)` returns the new reach and the walk stops when the next cell starts beyond it. Returns `GridRayWalk{cells, items}`.
- Mailboxing (`GridMarks`: per-item query numbers) reports an item in many cells once. The grid's own marks change in const queries, so one grid serves one thread; `walkRay` and the level's `raycast` also take a caller's `GridMarks`, so threads can walk one grid at once (the sky visibility bake does).

## `CollisionScene`

`physics/collision_scene`: a snapshot of every `Collider` + `Transform` (`SceneCollider`: world shape, model matrix, detail) in a `UniformGrid`.

- `rebuild(world)` clears and refills, reading its `CollisionSettings` each time. Rebuilt rather than updated because the ECS tracks no changes, so incremental updates could go stale.
- `raycast(scene, ray, detail, maxDistance, stats)`: narrow phase per visited collider; a triangle miss leaves the reach unchanged.
- `overlapping(scene, shape)`: shapes only, detail not consulted.
- `CollisionQueryStats{tested, cellsWalked}` (`physics/query_stats`) reports a query's cost (the demo's F4 lines).

## Static level

`StaticLevel` (`physics/static_level`): everything that never moves, as one world-space `TriangleMesh` with its own `UniformGrid` over triangle numbers, so static triangles are never re-inserted each tick.

- `add(piece, model)` validates first (a bad index changes nothing), then inserts; never rebuilt. Cell size from the `CollisionSettings` given at construction. To change the level, build a new one.
- `raycast(level, ray, maxDistance, stats)` → `LevelHit{hit, triangle}`; an overload takes `GridMarks` for threads.
- `contacts(level, capsule)`: grid query by `boundsOf`, one `Contact` per touched triangle.
- `sweepSphere(level, sphere, motion)`: grid query by the box round start and end spheres → `LevelHit`.

### Triangle contacts

`contact(capsule or sphere, a, b, c)` (`physics/triangle_contact`) → `Contact{point, normal, depth, triangle, faceNormal}`:

- Normal from the triangle's nearest point to the core; depth = radius - distance. Touching counts (depth 0).
- Core through the triangle: the face normal on the core middle's side; depth = deepest end behind the plane + radius.
- Triangles are two-sided.
- `faceNormal`: the triangle's own normal turned to the shape's side; differs from `normal` on an edge or corner, which is how the controller tells a ledge from a slope.

### Sphere sweeps

`sweepSphere(sphere, motion, a, b, c)` (`physics/raycast`): the centre's path raycast against the triangle grown by the radius (two offset faces plus a `Capsule` per edge).

- Hit `point` = centre at the touch; normal from the triangle to the centre.
- Already touching: distance 0 with the contact normal if moving into it (more than `glancing`, 1e-5), ignored otherwise, so a resting ball can roll off.
- Not built: box against triangle, capsule and box sweeps.

## Particles

`physics/particle`: a small ball flying under gravity and bouncing off a `StaticLevel` (grenades, later debris). `Particle` = `ParticleSettings` + `velocity`; the `Transform` position is the centre. Fields and defaults (a small hard ball on stone): the header; `visit` exposes them.

- `fly(velocity, acceleration, seconds)` → `FlightStep`: moved by the mean of start and end velocities, exact for constant acceleration at any step length.
- `bounceVelocity(velocity, normal, settings)`: normal part returned times `restitution` (none below `restingSpeed`); sliding part slowed by at most `friction` (1 + e) times the speed in, never reversed, so resting contact decelerates at mu g. A leaving velocity is unchanged.
- `moveParticle(particle, centre, level, seconds)`:
  1. Push out of overlaps, deepest first (up to 4 passes), ending `clearance` (1e-3) clear.
  2. Tentatively add gravity; if that moves it into a surface within 2 `clearance`, the surfaces push back (`pressAgainstTouching`) and it moves without gravity; otherwise it flies under gravity.
  3. Sweep the flight; at each hit stop `clearance` off, bounce with the velocity at the hit (share of path stands in for share of time) and fly on, up to `maxBounces`.
- A resting ball stays exactly still. `moveParticles(world, level, seconds)` for every `Particle` + `Transform`.

## Signed distance fields

`physics/signed_distance_field`: a closed mesh's shape as a grid of signed distances, so any body answers "how far, which way out" like a sphere's formula. Used by cloth only (`DistanceFieldCollider`; `cloth.md`). Compiled `-O2` in debug builds too. Tests: `signed_distance_field_test` (parked with the cloth).

- **Grid** (`SignedDistanceField`): `origin`, `spacing`, `counts`, `band`, `distances` (x fastest); `at(x, y, z)`, `bounds()`. Within `band` of the surface the exact distance (positive outside); past it `band` with its sign.
- **Build** (`buildSignedDistanceField(mesh, spacing, band, jobs)`): over the mesh's box grown by band + one spacing, so the edge is outside and far. Each triangle writes into the samples within band of its box, keeping the nearest (a triangle reaches a few thousand samples; cheaper than each sample searching triangles). Slabs of 8 samples along y (a standing body's longest side), a job each, triangles in order: the same field to the bit on any thread count. Throws for spacing ≤ 0, band < spacing, or no triangles.
- **Sign** (Baerentzen & Aanaes 2005): the offset from the nearest point along that feature's angle-weighted pseudo-normal (face: its normal; edge: sum of both faces'; corner: faces' weighted by angle). Exact inside/outside for a closed mesh. Unreached samples: a flood fill from the box's edge marks outside; the rest inside.
- **Sample** (`sampleField` → `FieldPoint{distance, gradient}`): `fieldCell` (cell and share; false outside the box) then `blendFieldCell` (trilinear as nested a + (b - a) t steps, so equal corners give exactly their value and zero slope; gradient = the blend's derivative over the spacing). Outside the box: `band`, no slope. Both mirrored in `cloth.glsl`.
- **Error:** exact at samples; between them a small fraction of the spacing near the surface. A cell's far corners clip at the band, so keep the band wider than cloth thickness plus a cell diagonal.
- **Not a `Collider`:** the player, picking and physics queries never see it. Shared pointer, since fields are large (the mannequin's about 6 million samples at 5 mm, from `assets/models/human.obj`).

## Character controller

`physics/character_controller`: a kinematic capsule moved by commands and kept out of a `StaticLevel`. Tuning (`CharacterControllerSettings`, the header): capsule radius and height, max slope 45 degrees, step height and snap distance 0.3, gravity, terminal fall speed and jump speed from community-measured Overwatch values, ground and air accelerations as feel-tuned guesses, `substepFraction`, `pushOutPasses`. Changing any of them: `.claude/rules/character-tuning.md`.

**Component** `CharacterController`: settings; commands `wishVelocity` (horizontal; vertical ignored) and `wantsJump` (used up by the next tick, honoured only on the ground); state `velocity`, `grounded`, `groundNormal`, `lastSubsteps`, `groundKnown` (a new controller probes the ground on its first tick). The `Transform` position is the feet. `controllerCapsule(settings, feet)` gives its capsule.

**`moveCharacter(controller, feet, level, seconds)`:**

1. Jump first, so take-off uses air rules.
2. Steer: on the ground `moveTowards` the wish at `groundAcceleration`; in the air no wish keeps momentum, and steering at `airAcceleration` keeps the speed (up to the wished speed) for turns up to 90 degrees.
3. Vertical velocity by semi-implicit Euler with quadratic drag while falling: `a = -g + (g / v_t^2) v^2`, v_t = `maxFallSpeed`.
4. On the ground, the horizontal velocity is tilted into the ground plane and rescaled, so slopes are walked at full speed.
5. Substeps of at most radius x `substepFraction` (at most 64), so walls cannot be skipped; each ends with `pushOut`: deepest contact first, re-queried each pass (corners need one per wall).
   - A contact that `supports` pushes straight up by depth / normal.y and stops the fall: no sliding on slopes (kinematic static friction).
   - Others push along the normal and remove velocity into it (frictionless, perfectly inelastic). While walking, steep upward normals push only sideways, so a steep ramp cannot be climbed.
6. If grounded and not jumping, `descend` follows the ground down up to `snapDistance` in 0.05 drops (`descentIncrement`), pushing out at the first overlap; this also removes hovering. Small drops keep a kerb's edge from winning the push-out sideways.
7. `groundUnder` (supporting contacts of the capsule lowered 0.01, `groundProbeDistance`) sets `grounded` and `groundNormal`.

**`supports`:** a walkable face (normal.y ≥ cos maxSlope), or an edge or corner contact (normal differs from `faceNormal` past cosine 0.999) facing up at least `leastStepNormalY` (0.05) and no higher than `stepHeight` above the feet. The capsule thus rides up low ledges with no separate step-up move.

**Also:** `characterFits(settings, feet, level)` (overlaps nothing; touching is fine; for standing up from a crouch); `moveCharacters(world, level, seconds)` for every `CharacterController` + `Transform`. `degreesFromRadians` is in `math/angles`.

## Tests

`overlap_test`, `closest_points_test`, `raycast_test`, `collision_mesh_test`, `collider_test`, `collider_raycast_test`, `uniform_grid_test`, `collision_scene_test`, `static_level_test`, `triangle_contact_test`, `particle_test`, `character_controller_test`, `signed_distance_field_test`.
