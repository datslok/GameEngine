# Collision and movement

What the engine knows about solid things: shapes and their overlaps, rays, picking, the broad phase, the static level, the character controller, bouncing balls, and how their cost is measured.

All of it lives in `physics/` (with the closest-point geometry in `math/` and mesh conversion in `scene/`). It is plain C++ with no GPU, so it runs inside the fixed-step simulation, headless and in tests. The exact names, fields and edge cases are in [the physics reference](../architecture/physics.md).

## Shapes: a core plus a radius

`physics/shapes` has three shapes:

- `Sphere`: a centre and a radius.
- `Capsule`: a segment `a`-`b` thickened by a radius, like a pill. With `a == b` it is a sphere.
- `Aabb`: an **axis-aligned bounding box**, a box whose faces line up with x, y and z, so two corners (minimum and maximum) describe it.

`Shape` is a `std::variant` of the three: one value holding exactly one of them, and knowing which.

The idea that makes rounded shapes cheap: each is a **core** (a point, or a segment) wrapped in a sheath of constant thickness. Two sheaths touch exactly when the cores come within the sum of the two thicknesses. So every overlap between rounded shapes is the same question in disguise: how close do two cores get?

Those closest-point questions are answered in `math/closest_points`:

| Question | How |
|---|---|
| Point to segment | Project onto the line, `t = (p - a)·d / (d·d)` with `d = b - a`, then clamp `t` to 0..1 so the answer stays on the segment |
| Point to box | Clamp each coordinate into the box's range (a point inside stays put, distance 0) |
| Segment to segment | The squared distance between `a1 + s d1` and `a2 + t d2` is a quadratic bowl in `s` and `t`. Set both partial derivatives to zero, clamp to the unit square, and re-solve along the clamped edge; parallel segments, where the equations go degenerate, get their own branch (Ericson, *Real-Time Collision Detection*, 5.1.9) |
| Segment to box | Along the segment, the squared distance to a box is piecewise quadratic: its formula changes only where the point crosses one of the box's six planes. Cut the segment at those crossings and minimise each piece exactly. No iteration, and a segment through the box gives 0 |
| Point to triangle | Which Voronoi region of the triangle's plane the point falls in decides whether the face, an edge or a corner is nearest (Ericson 5.1.5) |

Two conventions run through all of it:

- **Touching counts as overlapping.** Distance exactly equal to the summed radii is a contact. A character standing exactly on the floor has to be *on* it.
- **Squared distances.** `distance² ≤ (r1 + r2)²` gives the same answer as comparing distances, with no square root. Most tests work with squares.

## Raycasts

A **ray** is `p(t) = origin + t × direction`. A raycast (`physics/raycast`) asks for the first `t ≥ 0` where it meets a shape, and returns a `RayHit`: the distance in world units, the point, and the surface normal, always turned to face back against the ray, so a caller never has to ask which side it hit. A ray starting inside a shape hits at distance 0: it is already touching.

- **Sphere.** Put the ray into `|p - c|² = r²` and you get a quadratic in `t`. A negative discriminant misses; otherwise the smaller root is the entry.
- **Capsule.** Ignore motion along the axis and the side becomes a circle: remove the axial part of the direction and of the offset, and solve the same quadratic in the plane. That hit counts only between the segment's ends; the two end spheres are tested too, and the first hit wins.
- **Box (slabs).** A box is the overlap of three slabs, one per axis. For each, work out when the ray enters and leaves it. The ray is in the box while it is in all three: it enters at the *latest* entry and leaves at the *earliest* exit, and hits if the entry comes first. The face hit is on the axis of the latest entry.
- **Triangle (Möller-Trumbore).** Solve `origin + t d = a + u (b - a) + v (c - a)` for `t`, `u`, `v`: three linear equations, solved with cross and dot products. A hit needs `u ≥ 0`, `v ≥ 0`, `u + v ≤ 1` (inside the triangle) and `t ≥ 0`. Both sides count.

One bug is worth remembering. A ray running exactly along the edge two triangles share could slip through both, because rounding put it a hair outside each. The inside test therefore allows a tiny slack (`edgeTolerance`, 10⁻⁵), and that very ray is kept as a regression test: a test that pins a bug once fixed so it cannot quietly return.

A mesh raycast tests every triangle and keeps the nearest. `TriangleMesh` (`physics/triangle_mesh`) is the collision form of a mesh, positions plus three indices per triangle, built from a drawn mesh by `collisionMesh` (`scene/collision_mesh`).

## Colliders and picking

A `Collider` component (`physics/collider`) gives an entity a shape in its **own** coordinates, and optionally `detail`: its exact triangles, shared between every entity using the same model. The shape must enclose the triangles; it is the cheap stand-in tested first.

Before testing, the shape is carried into the world by the entity's model matrix (`worldShape`):

- **Points** (centres, segment ends) go through the matrix exactly.
- **Radii** grow by the largest stretch the matrix applies, the longest column of its 3x3 part. A squashed sphere is an ellipsoid, not a sphere, so the engine takes the sphere around it: always safe, exact under uniform scale.
- **Boxes** turned by a rotation stop being axis-aligned, so the world box is the axis-aligned box *around* the turned one. Its half-size on axis `i` is `Σ_j |A_ij| h_j`: each half-edge vector reaches its full length along `i`, whichever way it points.

Clicking (`raycastColliders`, `physics/collider_raycast`) runs in two stages. The ray is tested against every world shape; in `Triangles` mode each shape hit is then confirmed against the detail triangles, while `Shapes` mode takes the shape hit as the answer. Picking by shape is cheap and forgiving: one quadratic says whether you clicked the duck, and a click between two of its feathers still counts.

The pruning rests on one inequality. Since the shape encloses the triangles, the ray cannot reach a triangle before it reaches the shape: **the shape's hit distance is a lower bound on the triangles'.** Once a triangle hit is known at distance `d`, any collider whose shape is first hit beyond `d` cannot hold anything nearer, so its triangles are never tested. The query's reach shrinks as it goes.

The triangles stay in the entity's coordinates and the *ray* is carried to them, which needs the model matrix's inverse. For a matrix that only rotates, scales and moves, `[A t; 0 1]`, the inverse is `[A⁻¹, -A⁻¹ t; 0 1]` (`affineInverse` in `math/mat4`), with `A⁻¹` from the same cofactors as the normal matrix. Under scale, lengths along the ray differ inside and out, so the hit distance is converted back by the world length per local length, and the normal returns through the normal matrix. A ray slipping between triangles (past the duck's beak) carries on to whatever is behind.

## The broad phase: skipping most of the world

Testing a ray against every collider costs time in proportion to their number. A **broad phase** finds the few that could matter; the exact tests that follow are the **narrow phase**.

The engine's broad phase is a **uniform grid** (`physics/uniform_grid`): space cut into 2 m cubes (`CollisionSettings::gridCellSize`, about a character), like 3D graph paper. Each item goes into every cell its box touches. Only occupied cells are stored, in a hash map from cell coordinates to items, so empty space and a world of any size cost nothing. Items spanning more than 64 cells (the ground) go on a separate large list every query checks.

- **Region queries** look only in the cells a box touches. An item in several cells would be found several times, so each remembers the number of the last query that saw it and is reported once (**mailboxing**).
- **Ray walks** visit cells near to far, stepping across whichever cell boundary the ray meets next (Amanatides and Woo: the 3D version of drawing a line on pixels). The walk stops once the next cell starts beyond the nearest hit so far, since nothing further can beat it.

A query's mailbox marks are written as it runs, so one grid serves one thread at a time, unless each thread brings its own marks (`GridMarks`), as the sky visibility bake does when it walks the level's grid on every core.

`CollisionScene` (`physics/collision_scene`) holds a grid of every `Collider`, and is **rebuilt from scratch** whenever it is used. That sounds wasteful, but the ECS tracks no changes, so a grid patched piece by piece could keep stale entries for things that moved or died; a rebuild is always right, and cheap at these sizes. Tests check every query against brute force on random scenes.

## The static level

Things that never move (ground, walls, the obstacle course) form a `StaticLevel` (`physics/static_level`): all their triangles merged into one world-space `TriangleMesh` with its own grid over triangle numbers. Pieces are added once and it is never rebuilt; to change it, build a new one.

It answers raycasts, and for movement it answers **contacts**: every triangle a capsule or sphere touches, as a `Contact` (`physics/triangle_contact`) with the point, the normal pointing out towards the shape, the overlap depth, and the triangle's own face normal turned to the shape's side.

- The depth is the radius minus the distance from the core to the triangle: closest points again.
- A core piercing the triangle is pushed out along the face normal, on the side where its middle lies.
- On a face, `normal` and `faceNormal` agree; on an edge or corner they differ. That difference is how the character controller tells a ledge from a slope.

Spheres can also be **swept**: `sweepSphere` finds the first moment a moving sphere touches the level. A sphere's centre touches a triangle exactly when it reaches the triangle *grown* by the radius: two copies of the face pushed out along the normal, plus a capsule round each edge. So a sweep is one raycast of the centre's path against that grown shape. Nothing passes through a wall at any speed, which stepping alone cannot promise.

## The kinematic character controller

A real body pushed into a wall feels a contact force, decelerates and bounces a little. Game characters are usually **kinematic** instead: the code decides how they move, and collision only corrects the position. That gives the precise control a shooter needs: stop on a coin, no sliding on slopes, no bouncing off walls.

`physics/character_controller` moves a capsule (radius 0.4 m, height 1.8 m; the entity's position is the feet) through the static level each tick. Gravity, jump speed and terminal speed come from community measurements of Overwatch; the accelerations are tuned by feel. One tick, in order:

1. **Jump**, if asked and on the ground: vertical speed 5.72 m/s. It comes first so the take-off tick already uses air rules.
2. **Steer.** On the ground the horizontal velocity moves towards the wished one at 100 m/s² (full speed in a twentieth of a second, and the same to stop). In the air, wishing nothing keeps momentum; steering moves towards the wish at 30 m/s², and a turn of 90° or less keeps the speed rather than bleeding it on the way round.
3. **Fall.** Gravity is 17.5 m/s² (game feel, not 9.81), with quadratic drag while falling: `a = -g + (g / v_t²) v²`. That is Newton's law with drag proportional to speed squared, which approaches the terminal speed `v_t` = 30 m/s as `v(t) = v_t tanh(g t / v_t)`. It is integrated by **semi-implicit Euler**: update the velocity, then move with the *new* one. Explicit Euler, moving with the old velocity, slowly pumps in energy, so jumps would creep higher.
4. **Follow the slope.** While walking, the horizontal velocity is tilted into the ground's plane and rescaled to full length, so a slope is walked as fast as flat ground.
5. **Substeps.** The move is cut into steps of at most half the radius (at most 64 a tick). A step longer than the capsule is thick could hop clean over a thin wall: the classic **tunnelling** problem.
6. **Push out** after every substep: find the deepest overlap, push out of it, ask again (escaping one wall can push into another, as in a corner), up to 8 times. The push depends on what was touched:
   - **Ground**, a walkable face up to 45°, pushes straight **up** by `depth / normal.y`, the height that clears the same overlap, and stops the fall. Pushing along the normal would slide the character downhill a little every tick; going straight up is the kinematic stand-in for static friction.
   - **Walls and ceilings** push along the normal and remove the velocity going into them: a frictionless, perfectly inelastic collision. Motion into the wall stops, motion along it continues, so the character slides along walls. While walking, a steep upward face pushes only sideways, so a steep ramp cannot be climbed.
   - **Steps.** When a round body meets a ledge of height `h`, the contact is on the ledge's *edge*, and its normal sits at `acos((r - h) / r)` from vertical, often steeper than any walkable slope. So an edge or corner contact no higher than the step height (0.3 m) above the feet also counts as ground and pushes up. The capsule rides up kerbs and stairs by itself, with no separate step-up move; a steep ramp is still a wall, because walking into it touches its *face*.
7. **Settle.** A character that was walking and did not jump is lowered up to 0.3 m in 5 cm drops, so it follows the ground down a step or over a ramp's crest instead of launching into the air, and any hover a push-out left disappears. Small drops keep a kerb's edge from winning the push sideways.
8. **Look underfoot.** A copy of the capsule lowered by 1 cm decides `grounded` and the ground's normal.

The tuning is settled; changing any of it follows `.claude/rules/character-tuning.md`.

Two things drive the controller (details in [the gameplay reference](../architecture/gameplay.md)):

- The **first-person body** (`gameplay/first_person_body`), an invisible capsule. Each frame WASD becomes a wished velocity turned by the camera's yaw: 5.5 m/s, 90% straight back, 3 m/s crouched, 1.5 times while sprinting. Crouching shortens the capsule at once (shorter always fits); standing up waits until the full capsule fits. The camera rides at the eyes, which ease between standing and crouching height over a tenth of a second.
- The **MOBA character** (`gameplay/character`), steered by its click-to-move target.

## Particles: balls that bounce

A `Particle` (`physics/particle`) is a small ball that flies and bounces off the static level: the demo's grenades (G).

- **Flight** moves by the mean of the start and end velocities, `Δx = v t + ½ a t²`. That is the exact solution for constant acceleration, so a throw follows the true parabola however long the tick.
- **Collisions are swept, not stepped.** Each tick's flight is swept through the level; at the first surface the ball stops just clear, bounces, and flies on for the rest of the tick, up to 4 surfaces.
- **A bounce** splits the velocity into the part into the surface and the part along it. The normal part comes back `e` times as fast, the **coefficient of restitution** (0.45; 0 sticks, 1 bounces fully). Coulomb friction then slows the sliding part by at most `μ` (0.4) times the change in normal speed, never reversing it. For a ball resting on a floor this is exactly the kinetic-friction deceleration `μ g` of the textbook block on a table.
- **Coming to rest.** Arriving slower than 1 m/s does not bounce, so the bouncing dies out instead of halving forever (Zeno's ball). A ball pressed against the ground has gravity cancelled by the surface, so a resting ball stays exactly still, with no jitter.

Particles fall at a real 9.81 m/s², unlike the character.

## Distance fields: bodies of any shape

Cloth needs to collide with a human body, which no sphere or capsule approximates well. `physics/signed_distance_field` stores a closed mesh as a 3D grid of **signed distances**: at each sample, how far the nearest surface is, positive outside and negative inside. Reading it trilinearly between samples gives the distance and, from its slope, the direction out, for any point: the same answer a sphere's formula gives, for any shape. Only the cloth uses it; see [the cloth reference](../architecture/cloth.md).

## Measuring cost: the profiler and the release build

Physics was the first work whose cost grows with the scene, so it came with a way to measure. `Profiler` (`core/profiler`) times named sections with `std::chrono::steady_clock`, a clock that only runs forwards at a constant rate (the wall clock jumps when the computer corrects its time).

- It counts **per call**, not per frame, because ticks do not fall one per frame: at 240 frames a second and 120 ticks, half the frames run no tick.
- It collects for half a second before showing anything, then shows that window's average and its **worst** call. The digits stay readable, and a hitch you can feel is not averaged away.

F4 shows the frame, the work (the frame minus the frame cap's sleep), the ticks, update, scene and render times, and each demo system's cost.

Those numbers show why the **release build** matters. The debug build compiles with `-O0`, so a debugger can stop on any line and see any variable. The price: every variable lives in memory, nothing is simplified, and every function call really happens. `velocity + acceleration * seconds` becomes two calls, each copying three floats in and out, to do six multiply-adds.

`make release` uses `-O2`: values stay in registers, repeated work disappears, and small functions are **inlined** (their body pasted at the call). The compiler can only inline a body it can *see*, so the small vector operations live in their headers (`math/vec3.h` and friends): a body in a separate `.cpp` file is invisible while compiling the files that call it.

Per tick, 40 characters walking a stepped level and 200 bouncing balls:

| Build | 40 characters | 200 balls |
|---|---|---|
| Debug (`-O0`) | 0.52 ms | 0.54 ms |
| `-O2`, vector operations in `.cpp` files | 0.105 ms | 0.090 ms |
| `-O2`, vector operations in headers | 0.075 ms | 0.067 ms |

Seven to eight times faster in all; the headers' share, 1.4 times, was the cost of the calls themselves. Link-time optimisation (inlining across files at link time) added only 3 to 9% while a one-file rebuild went from 3 to 20 seconds, so it is not used. The builds themselves: [the build reference](../architecture/build.md).

## C++ met along the way

- **`std::optional<T>`**: a value that may be missing. A raycast returns `std::optional<RayHit>`, either a hit or nothing, with no magic "-1 means missed". `if (hit)` checks; `hit->distance` reads.
- **`std::variant` and `std::visit`**: `Shape` knows which of its three it holds, and `std::visit` calls the right function for it, so `overlaps(Shape, Shape)` picks among the nine pair tests in one line.
- **Overloading**: one name, several functions told apart by argument types. `raycast(ray, sphere)` and `raycast(ray, mesh)` read as one idea; the compiler picks.
- **`std::shared_ptr`**: shared ownership. A detail mesh is held by every entity using it and freed when the last lets go; the `const` in `shared_ptr<const TriangleMesh>` stops any one of them changing it under the others.
- **Floating-point tolerances**: a float carries about 7 significant digits, so values that should be equal often differ in the last bit. The physics compares against small named tolerances (`edgeTolerance`, `touchingDepth`, `clearance`) rather than `==`, and a ball stops exactly 1 mm (`clearance`) off a surface so rounding cannot leave it inside.
- **RAII for timing**: `ScopedTimer` reads the clock when made and records the difference when destroyed. C++ runs destructors whenever a block ends, however it ends, so the stopwatch can never be left running: the same idea that frees SDL resources. `timeSection(profiler, section, [&] { ... })` wraps it round a **lambda**, a small unnamed function written in place.
