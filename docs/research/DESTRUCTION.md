# Destruction and flight: plan

Notes and a build plan for a fifth kind of game: a superhero who flies fast enough to feel it and is strong enough to bring down whole buildings, which fall by real physics.

The research half records what shipped engines and papers do, with their numbers where they publish them. Each part then says **what it means here**: what the engine already has, what must be built and how it is tested.

The roadmap items are phase 15 in [ROADMAP.md](../ROADMAP.md). Started Oct 27, 2026, from the user's idea.

Facts marked **(from memory)** were not checked against their sources when this was written; check them before writing tests that depend on them.

## The idea in one paragraph

Two halves that feed each other. **Flight** makes the player fast: hundreds of metres a second, low between buildings, so the city streams past. **Destruction** makes the player strong: a building is not a scripted animation but a structure of chunks bonded together, carrying its own weight down to the ground. Break the right supports and the load shifts, the next supports overload, and it comes down floor by floor. Real scale ties them together: big things fall slowly (a 100 m tower takes about 4.5 s), which is what makes them look big, and next to a player moving at the speed of sound the collapse looks majestic.

```
 flight controller ──impacts (momentum, contact point)──▶ structure (chunks + bonds)
        ▲                                                     │ stress solver: bond forces
        │                                                     ▼
 camera feel, motion blur,                         bonds past their limits break
 wind, sonic boom, sound                                      │
                                                              ▼
                                       pieces cut off from the ground become rigid bodies
                                                              │
                                                              ▼
                                  rigid-body solver (XPBD substeps) ─▶ debris, dust, light probes
```

## What the engine has and lacks

**Has:**

- the fixed 120 Hz tick, substeps, the job system and XPBD (phase 7's cloth, on every core and on the GPU);
- sphere sweeps against the static level (phase 5), the start of fast collision;
- camera-relative rendering (phase 6a), reversed-Z depth and a per-pixel velocity image (TAA, phase 6e), which motion blur can read;
- light probes that react to big changes (`ProbeEvent`, phase 6d), volumetric fog (a froxel grid that could carry dust), ray tracing with a top-level structure rebuilt every frame;
- a renderer that draws thousands of instances of a mesh in one draw (the GPU scene, phase 6c).

**Lacks:**

- rigid bodies (phase 8 builds them, but capsules only, for ragdolls);
- boxes and convex shapes and their contacts;
- fracture, bonds and a stress solver;
- a flight controller and the camera feel of speed;
- a city: streaming, levels of detail and positions far from the origin.

## Part 1: Feeling fast

**Speed is perception, not the number.** The eye judges speed from how fast the scene streams past, not from the velocity:

- **Global optic flow** is speed divided by height above what you see. Flying at 300 m/s 1 km up looks like strolling; skimming rooftops at 30 m/s looks fast.
- **Edge rate** is how many edges (windows, lamp posts, road markings) pass a point of the view a second. Many small details passing close make speed.
- Both are from studies of driving and flight simulators (Larish & Flach 1990, **from memory**). The design follows: give the player things to fly close to, and detail on them.

**The tools,** most cheap for this engine:

| Tool | What it does | Engine cost |
|---|---|---|
| Field of view widening with speed | stretches the edges of the view, so the edge rate rises | a camera setting |
| Motion blur | each pixel smeared along its motion | reads TAA's velocity image; a reconstruction filter (McGuire et al. 2012, **from memory**) in one pass |
| Camera shake | small, fast, rising with speed and impacts | a camera setting |
| Wind streaks and speed lines | particles stretched along the motion near the camera | GPU particles (phase 14's particle effects) |
| Vapour cone and sonic boom | a cone of condensation near Mach 1 in humid air, a bang when passing it | a mesh and a sound |
| Dust wakes | the ground and water pushed aside when skimming low | particles, the fog's density |
| Sound | wind rising with speed, the Doppler shift of what you pass | phase 9's audio |
| Hit-stop | a few frames' pause on a big impact | the tick budget already allows slow motion |

**The flight controller** is tuned for feel, as the Overwatch movement was, not for aerodynamics:

- states: hover, cruise and boost, with a quick stop;
- turns limited by sideways acceleration (in g), not by turning rate, so the faster you go the wider you arc: it feels physical without lift and drag;
- banking into turns for the camera and the body, not the physics.

**Fast means far per tick.** At the speed of sound (343 m/s) the player moves 2.9 m a tick at 120 Hz; at Mach 3, 8.6 m. A plain overlap test misses walls: every move must be a sweep (phase 5's sphere sweep against the static level; against moving debris too, as phase 12's projectiles need).

**Positions far from the origin.** A float's step is about 0.5 mm at 4 km from the origin and 2 mm at 16 km. Rendering is already camera-relative; physics would want a floating origin (the world shifted back near zero when the player strays far) or phase 13's double positions. A city crossed at Mach 3 in 10 s also needs streaming (phase 14's open-world item).

## Part 2: Rigid bodies for debris

**The solver.** Extended position based dynamics for rigid bodies (Müller, Macklin, Chentanez, Jeschke & Kim 2020, Computer Graphics Forum 39(8)): positions and orientations stepped like cloth's points, constraints and contacts solved on positions with compliance, in small substeps (Macklin et al. 2019, "Small Steps"). Phase 8 builds it for capsules. Its multipliers give each constraint's force directly (force = lambda / h^2), which matters for Part 4.

**Shapes.** Chunks of a fractured wall are convex polyhedra (Voronoi cells), not capsules:

| Shape | Contacts | Pros | Cons |
|---|---|---|---|
| Boxes | separating axes | exact for bricks, slabs and beams; the simplest polyhedron | only for boxy chunks |
| Convex hulls | GJK for distance, EPA or separating axes for depth (**from memory**: the standard pair) | any Voronoi chunk | contact generation is the costly part, a few contact points a pair kept stable |
| Clusters of spheres (FleX-style, Macklin et al. 2014, **from memory**) | sphere against sphere | cheap and simple on the GPU | approximate, bumpy faces, many spheres for a slab |

**What it means here:** boxes first (beams, slabs, columns, and most of phase 14's crates), then convex hulls for the Voronoi chunks. Thousands of chunks need sleeping (bodies at rest stop being stepped until touched) and islands (only groups that touch are solved together), then the GPU, where phase 7's cloth passes already show the pattern: coloured constraint passes, one dispatch a colour.

**Fast impacts** between bodies: speculative contacts (a contact made before the bodies touch, if they could touch this step; Catto's continuous collision talks, **from memory**) keep debris from passing through floors at 30 m/s.

## Part 3: Fracture

**Pre-fractured chunks** are what shipped games use. A wall or slab is cut into pieces before play, and the pieces stay glued until the game breaks the glue:

- **Voronoi cells:** scatter seed points through the volume and give each the region nearer to it than to any other seed. Each cell is the solid clipped by the planes halfway to its neighbours, so it is convex. More seeds where hits are likely (near the ground, round columns) gives smaller pieces there.
- **Clusters:** Unreal's Chaos joins a cluster's mass and inertia into one larger rigid body until the bonds inside break, so an intact building costs one body, not thousands (Epic's destruction overview). Its connection graph is built from nearest neighbours, each connection a rigid constraint with a strain limit, broken when an impulse exceeds it.
- **The bond graph:** two chunks sharing a face are bonded; the bond's area is the shared face's area, its normal the face's normal. Chunks touching the ground or foundations are supports.

**Run-time fracture** cuts where the hit lands instead: Müller, Chentanez & Kim 2013 ("Real Time Dynamic Fracture with Volumetric Approximate Convex Decompositions", ACM Transactions on Graphics 32(4)) apply a fracture pattern at the impact point to a convex decomposition, partly fracturing large objects; their arena demonstration ends with 20,000 pieces at over 30 frames a second, rigid bodies, dust and rendering included. Later, if pre-cut lines look repetitive.

**Voxels** are the other road (Teardown): every voxel takes part, so anything breaks anywhere, and pieces cut off from the rest fall. Pros: no authoring, breaks anywhere. Cons: a blocky look, memory and cost that grow with volume (a skyscraper at 10 cm voxels is billions of cells), and cruder structural physics; its developers have shown a structural integrity system only as a prototype.

**What it means here:** Voronoi chunks cut at load from each building's blocks, clustered, with a bond graph. Run-time fracture and voxels stay options.

## Part 4: Why buildings collapse

**The building is a truss.** Each chunk is a node with a mass; each bond is a member that can push (compression), pull (tension) and slide (shear). In equilibrium every node's bonds hold its weight and whatever pushes on it:

- sum of the bond forces on a node + its weight m g + outside forces = 0 (and the same for torques);
- supports (the ground) take whatever reaches them.

A real building has more members than equilibrium needs (it is statically indeterminate), so the forces depend on stiffness: give each bond a spring stiffness k (from its area, length and the material's Young's modulus), solve for the small displacements u that balance the loads, K u = f, and each bond's force is its stiffness times its stretch. K is a weighted graph Laplacian, symmetric and positive definite once the supports are fixed, so conjugate gradients solve it.

**The stress** in a bond is its force over its area, split into the part along its normal (compression or tension) and the part across it (shear). Each is compared with the material's limits.

**What NVIDIA Blast does** (its stress solver, NvBlastExtStress and the Omniverse extension's settings):

- works on the bond graph of the support chunks; the main use is gravity on a static structure until bonds fail, and spinning actors add centrifugal force;
- external accelerations on the chunks are the input; it finds the internal bond forces needed to keep bonded chunks from moving relative to one another, turns them into pressures through each bond's area, and splits each into compression or tension along the normal and shear across it;
- each has an elastic limit (above it the bond takes damage) and a fatal limit (above it the bond breaks at once), in MPa; defaults 0.008 and 0.01 MPa for compression, tension and shear falling back to them;
- iterations a frame (25 by default), so the stress propagates over several frames, and graph reduction (a graph about 2^level times smaller), more fidelity for the same iterations but sparser cracks;
- force beyond a bond's limit goes to the separating pieces, so harder hits throw fragments faster.

Red Faction: Guerrilla (Geo-Mod 2.0, 2009) brought down whole buildings the same way: pre-broken meshes reacting to stress and impacts, so cutting a building's lower supports eventually brings it down (secondary sources only; no developer talk found).

**Real materials, real numbers** (typical textbook values, **from memory**):

| Material | Density | Compressive strength | Tensile strength |
|---|---|---|---|
| Concrete | 2,400 kg/m^3 | 20-40 MPa | about a tenth of that, 2-4 MPa |
| Structural steel | 7,850 kg/m^3 | 250-355 MPa yield | the same |
| Brick masonry | about 1,900 kg/m^3 | 5-20 MPa | almost none |

Blast's defaults are thousands of times weaker than concrete: tuned so game weapons can break things. Real values work for this game, because the player is the super-strong one.

**A worked example** (illustrative numbers): a ten-storey concrete frame, 20 m square, about 1 tonne per square metre of floor all in, weighs about 4,000 t, 39 MN. On 16 columns of 0.5 x 0.5 m at the base (4 m^2 in all) that is about 10 MPa: a third of the concrete's strength, a sensible safety factor. Knock out half the base columns and the rest carry 20 MPa. Done suddenly, an elastic member feels up to twice the static load (a weight dropped onto a spring stretches it twice as far as one set down gently), so 40 MPa for a moment: past the strength, and the next columns fail. That cascade is **progressive collapse**. Once a floor falls, the block above lands on the one below with its fall's energy, far more than any floor was built to stop, and it crushes down floor by floor (Bazant & Zhou 2002's analysis of the World Trade Center, **from memory**).

**How hard can the player hit?** Momentum: a 100 kg flier at the speed of sound carries 100 x 343 = 34 kN s. Stopped in 10 ms that is an average force of 3.4 MN, more than enough to snap a column needing about 0.75 MN in tension (3 MPa x 0.25 m^2). So the physics allows a supersonic punch to cut a column. By the same momentum the player would bounce off; how much they keep going is a game dial (an effective mass), not physics.

**Time is the scale.** A fall from height h takes the square root of 2h / g: 2.7 s for a 35 m building, 4.5 s for 100 m. A slender tower toppling about its base is an inverted pendulum, its time going with the square root of length over g. Get g and the sizes right and big things look big.

**The cloth lesson applies.** A tall building is a long load path from roof to foundation. Solving the bonds as stiff joints in the dynamic solver, one Gauss-Seidel pass a substep, carries load about one bond a pass (phase 7's yardstick: XPBD's stretch grew with the square of the points along the path), so the base would read the roof's weight wrong for many frames: false breaks, or a sagging tower. Statics has no inertia, so the classic cure works cleanly here: a multigrid or conjugate-gradient solve of K u = f, warm-started from last frame. Blast's graph reduction is the same idea, a coarser graph.

**What it means here:**

| Choice | Pros | Cons |
|---|---|---|
| **Bonds as stiff joints in the rigid-body solver** (forces from the multipliers) | one solver; forces for free | the whole building dynamic and costly; slow convergence down tall load paths; jitter |
| **A separate static solve on the intact graph** (Blast's way) | buildings cost nothing until hit; accurate with conjugate gradients or multigrid; spread over frames | a second solver; impacts approximated as forces over the contact's duration |

The second: an intact building is kinematic (it does not move), its bond graph solved for statics when something changes it (a hit, a lost bond), with impacts as forces over their duration. Bonds past their limits break; the pieces still joined to the ground stay part of the building, and each group cut off (found by union-find over the remaining bonds) becomes a rigid body (a cluster) that falls, still bonded inside until it hits something hard enough.

## Part 5: Debris, dust and light

- **Big pieces** are the clusters and chunks of Part 2. **Small debris** (gravel, glass) is particles that collide with the world but not with each other (phase 5's particles, on the GPU).
- **Dust** is the collapse's signature. The volumetric fog already lights a froxel grid round the camera; a dust density added to it (injected where chunks break and land, spreading and settling) gets lit by the sun and lamps for free. Particles for the near, fast puffs.
- **Light:** each collapse sends a `ProbeEvent` over its region, so the light probes restart there; moving chunks wake the probes near them; shadow tiles holding moving casters redraw. Each chunk is an instance of its piece's mesh, so ray tracing needs one bottom-level structure per piece shape and an instance per chunk in the top-level one, rebuilt every frame already.

## The plan

Each step is a branch of its own, explained first, tests first. It starts after phase 8's rigid bodies, and is better after phase 9's audio (sound is half of feeling fast).

1. **Convex rigid bodies.** Boxes, then convex hulls, on phase 8's XPBD bodies: contacts, friction, sleeping, islands. Tests: a box resting on a slope at its friction angle, a stack of boxes standing still, momentum through collisions, bit-identical runs.
2. **Fracture.** Voronoi cutting at load (seeds, clipping by bisector planes), the bond graph with shared-face areas and normals, supports from the ground. Tests: the cells fill the block exactly (volumes sum), each is convex, bonds only between cells sharing a face.
3. **The stress solver.** Statics on the bond graph by conjugate gradients, on the CPU. Tests against hand calculations: a column's base carries the weight above it; a cantilever's root bond carries the bending moment w L^2 / 2; a beam on two supports splits its load by the lever rule; removing a support moves its load to the others.
4. **Breaking.** Elastic and fatal limits, damage between them, islands cut off by union-find becoming clusters, the excess force into the pieces' velocity. Tests: a column pushed past its strength breaks there; the worked example's frame collapses when half its base columns go and stands when a quarter do.
5. **On the GPU.** Thousands of chunks and the stress solve as passes, measured on a ladder of building sizes, as the cloth yardstick does.
6. **Flight.** The controller, the camera's field of view and shake, motion blur from the velocity image, wind streaks and the sonic boom, sweeps at speed.
7. **A destruction lab.** A game folder (`games/destructionlab/`) with a few buildings of different structures (a concrete frame, a brick house, a steel tower) to fly at and judge; dust and debris.
8. **A city.** Streaming, levels of detail and a floating origin (phase 12's and phase 14's items, phase 13's precision), when the lab's buildings work.

## Sources

- [NVIDIA Blast, Stress Solver (NvBlastExtStress), Blast 1.1](https://archive.docs.nvidia.com/gameworks/content/gameworkslibrary/blast/1.1/api_docs/files/pageextstress.html)
- [NVIDIA Omniverse Blast, StressSolverSettings](https://docs.omniverse.nvidia.com/kit/docs/omni.blast/latest/_build/docs/omni.blast/latest/structcarb_1_1blast_1_1_stress_solver_settings.html)
- [Unreal Engine, Chaos destruction overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/destruction-overview)
- [Müller, Macklin, Chentanez, Jeschke & Kim 2020, Detailed Rigid Body Simulation with Extended Position Based Dynamics](https://diglib.eg.org/handle/10.1111/cgf14105)
- [Macklin et al. 2019, Small Steps in Physics Simulation](https://diglib.eg.org/handle/10.1145/3309486-3340247)
- [Müller, Chentanez & Kim 2013, Real Time Dynamic Fracture with Volumetric Approximate Convex Decompositions (SIGGRAPH history archive)](https://history.siggraph.org/?p=108389)
- [Geo-Mod 2.0 (Red Faction wiki, secondary)](https://www.redfactionwiki.com/wiki/GeoMod_2.0)
- [Teardown's structural integrity prototype (80 Level, secondary)](https://80.lv/articles/this-real-time-voxel-building-demolition-is-so-satisfying/)
- From memory, to check: Larish & Flach 1990 (optic flow and edge rate); McGuire, Hennessy, Bukowski & Osman 2012 (motion blur reconstruction filter); Macklin, Müller, Chentanez & Kim 2014 (unified particle physics, FleX); Catto's continuous collision talks; Bazant & Zhou 2002 (progressive collapse of the World Trade Center); the material table.
