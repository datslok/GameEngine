# Light that bounces

What global illumination is, how the engine fakes and measures it without ray tracing (screen bounce, ambient occlusion, sky visibility), and the ray-traced light probes, on hold.

## What is missing from direct light

Shading a pixel from each lamp and the sun gives **direct** light: source, surface, eye. Real light does not stop there. Sun through a window hits the floor, the floor returns a fifth or so of it to the walls and ceiling, they return some of that, and so on. That is why a room lit by one window is not black wherever the beam misses.

Adding all of it up is **global illumination**. Its oldest stand-in is the **flat ambient**: one constant light from every direction, everywhere. It is cheap and wrong in a telling way: a sealed room gets as much of it as an open field.

**What a matte surface needs.** A Lambertian surface's brightness depends only on its **irradiance** E, the light arriving over the hemisphere above it with each direction weighted by the cosine of its angle to the normal:

  E = ∫ L(ω) V(ω) cos θ dω

L is the light from direction ω, V is 1 where the way is open and 0 where something blocks it. Every method in this chapter estimates some part of that integral more honestly than the flat ambient does.

**Units.** A pixel value in this engine is π times the physical radiance, and a light's number is the irradiance it gives a surface facing it. The cosine-weighted average of L over a hemisphere is E / π, and the π in the pixel values cancels it, so whatever a method averages from pixels lands directly in the slot the flat ambient used, with no fudge factor. A test under a uniform sky of value B must give back B (the **furnace test**, the standard sanity check in rendering).

## What runs by default

Ray tracing is optional and off by default (`RenderSettings::rayTracing`, the master switch), light probes included: most players' GPUs either lack it or cannot spare it in a real game. So the default picture builds its indirect light from cheap pieces, each standing in for something rays would do (contact shadows, the fourth such piece, are in [Shadows](shadows.md)):

| Piece | Stands in for | Where it comes from |
|---|---|---|
| Ambient occlusion | the V in the integral, at small scale | the depth image already drawn |
| Screen bounce light | light off nearby surfaces | last frame's finished picture |
| Sky visibility | V at room scale, plus a bounce off the walls | rays on the CPU against the static level |

The look is judged with the **typical GPU setup** on (`render/gpu/typical_gpu`: the High preset, ray tracing off, DLSS or FSR or TAA at Quality; `typical-gpu` command, `--typical-gpu`).

## Ambient occlusion: the darkness in creases

**The gap it fills.** Anything that supplies L for the integral above (the flat ambient, or probes half a metre apart at best) cannot see that the floor at a wall's foot has half its sky taken by the wall, or that the slot under a block is nearly closed. Without that, creases are as bright as open floor and objects look as if they float.

**Ambient occlusion** is V for nearby blockers: the cosine-weighted share of the hemisphere left open within a short radius (0.5 m by default, matched to the densest probe spacing so the two never count one blocker twice). Open ground scores 1, the foot of a tall wall 0.5, a sealed crack 0. Only the light from all around is multiplied by it; the direct light already has its own shadows.

**Horizons from the depth image** (ground-truth ambient occlusion, Jimenez and others, 2016). Rather than fire rays, the pass reads the depth the prepass drew. Pick a **slice**: a plane through the point that contains the line of sight, turned about it by some angle. On screen it is a line through the pixel. Walk it both ways reading depths; on each side the highest surface reached is a horizon, and everything beneath it is blocked.

**An exact integral per slice.** Between the horizons the slice is open, and the cosine-weighted open arc has a closed form. From each horizon angle h, with n the normal's angle within the slice, the open part contributes

  ¼ (cos n + 2h sin n − cos(2h − n))

An extra |sin θ| inside the integral accounts for the slice's share of the sphere, just as sin θ appears in dω = sin θ dθ dφ. Averaging slices turned all round the line of sight gives the exact visibility, so the only error left is what depth cannot show: the backs of things.

**A visibility bitmask** (Therrien and others, 2023). A horizon only says "everything below this angle is blocked", which treats a thin pole as a solid wall reaching to infinity behind it. Instead each slice is split into 32 sectors carrying equal shares of cosine-weighted light (one bit each in a 32-bit word), and each sample marks only the sectors its own assumed thickness (0.25 m) covers. Light passes behind thin things, and a sector already marked is never counted twice.

**The surface's normal from depth alone.** The pass rebuilds the normal from neighbouring pixels, picking on each side the neighbour whose next neighbour continues in a straight line: on a plane, pixels in a line are points in a line. Picking the nearer neighbour fails at a wall's foot, where the floor is so foreshortened that the wall's pixel above is closer than the floor's pixel below.

**Noise, averaged then cleaned.** Each pixel takes three slices of six steps a frame (fewer on lower presets), turned differently every frame by a blue-noise tile, so leftover error is fine grain rather than blotches.

- With anti-aliasing on, each pixel keeps a running mean of up to 16 frames, reprojected as the camera moves; history that lands on a different depth is thrown out, and a history that strays too far from the current frame's neighbourhood is pulled back or restarted, so a light switched off does not linger.
- An edge-aware **à-trous** blur ("with holes": one 5 x 5 kernel spread 1 and then 2 pixels apart) cleans the rest, weighted so nothing bleeds across a depth step or a corner.
- By default all of it runs at **half resolution**: each half-size texel takes the exact depth of one of its four pixels, a different one each frame, and an upsample afterwards keeps every pixel to texels on its own surface.

**Two fits from path-traced references.** A crease between white walls is lighter than visibility alone says, because light bounces inside it; Jimenez fitted that as a cubic in visibility and albedo, so bright creases darken less (used while no bounce light is gathered, below). Reflections in a crease are dimmed too, by Lagarde's specular occlusion.

Exact settings, passes and images: [ambient-occlusion.md](../architecture/ambient-occlusion.md).

## Light from the screen: bounce without rays

The picture finished last frame *is* the light leaving every visible surface towards the camera. So a pixel can gather its bounce light from the pixels around it, at no tracing cost.

- **The same walk.** The occlusion pass already marks, for each slice, which sectors a nearby surface blocks. When no probes are in the frame and anti-aliasing is on, it walks further (2 m) and, for each newly blocked sector, reads that surface's colour from last frame's picture. Light that would have come from the sky now comes off the wall: a red wall tints the floor beside it.
- **Sectors count light fairly.** Each sector is an equal share of cosine-weighted light, so a blocker's colour times its count of fresh sectors over 32 is its exact contribution, and open sectors keep the light from all around. A screen of uniform white gives back exactly the light the occlusion removed: the furnace test again.
- **Far samples read wide.** A sample several pixels apart stands for the whole stretch between samples, so it reads a blurred level of a **colour pyramid** (a mip chain of the picture, each level averaging 2 x 2 texels of the one below) instead of a single texel. The reflections reuse the same pyramid.
- **Fog taken back off.** The picture has the fog's glow in it, which is not light the surface sends; each sample has the fog in front of it undone first.
- **Many bounces for free.** Last frame's picture already holds last frame's bounce, so every frame adds one more bounce, a geometric series in the albedo that converges because the albedo is below 1.
- **Noise.** It always takes the denoiser, since bounce light is soft and its noise is not detail.

**What it cannot see.** Only what is on screen gives light: turn away from a sunlit wall and its bounce goes with it, and specular highlights in the picture bounce as if they were matte. The room-sized part of the problem is the next section's.

## Sky visibility: rooms that go dark

The flat ambient is mostly the sky's light arriving from everywhere. Deep in a room hardly any sky is visible, yet the flat ambient lights it as brightly as a field. Without probes the engine answers the question ahead of time: how much sky can each point see?

**A grid baked on the CPU.** Points every half metre round the camera (48 x 24 x 48 of them), with a second cascade twice as coarse beyond. Each fires rays against the static level's own triangles, on every core, in stages:

| Stage | Rays a point | Why |
|---|---|---|
| Quick | 64 | a new grid fills fast |
| Refine | 256 | the steady result |
| Deepen | 2,048 | points that see the sky through only a few rays (a window seen from far inside a room): with k escaping rays the share is known to about 1/√k |

A ray meeting the back of a triangle votes "this point is inside a wall"; if over a quarter do, the point is buried and never read.

**Two harmonic bands.** Each point stores the open share over directions as the first two spherical harmonic bands, a constant a and a lean b, so V(ω) ≈ a + b·ω: the monopole and dipole of a field. That is enough for "more sky above than below, more towards the window". A surface facing n sees a + ⅔ b·n of the sky (the ⅔ is the cosine-weighted average of ω over a hemisphere); a shiny one reads along its mirror direction; the fog reads a alone.

**Bounce off the blockers.** A ray that misses the sky hits something lit by the sky too. The hit brings back its triangle's albedo times the sky light the grid itself gives at that spot, so walls light each other and a corner matches its walls instead of going black; as the grid refreshes, light keeps bouncing. All of it is kept relative to one unit of flat ambient, and the shader scales it by the current sky light, so dawn or dusk costs no new bake. Bounced sunlight is missing here; on screen, the gathered bounce light supplies it.

**Leaks.** Like probes, a lookup blends the eight points round a pixel. A cheap guard stands in for the probes' distance test: each point stores how far it can see along the six axis directions, and a corner whose way to the lookup point is blocked is left out. Walls mostly run along the axes, so this catches most leaks.

**Following the camera.** The grid is a window on a lattice fixed in the world, kept as a circular buffer (as the probe cascades below), so a move only bakes the plane of points it brings in. That bake runs on a low-priority job system of its own and never holds up a frame; until a point is ready, the coarser cascade or an "open field" guess stands in. Every baked point is also kept by its place in the world (256 MB by default), so walking back finds it ready.

**Limits.** Only the static level counts: an opened door or moved crate changes nothing.

Exact layout, stages and readers: [sky-visibility.md](../architecture/sky-visibility.md).

## The light probes (ray traced, on hold)

With ray tracing on (and the probes' own switch, `GlobalIlluminationSettings::enabled`, both off by default), a grid of **light probes** measures the bounce light instead. The method is DDGI, "dynamic diffuse global illumination" (Majercik and others, 2019, and its 2021 production version). The probes are on hold until the ray and path tracing phase takes them up; their tests are parked. This is how they work.

**The idea.** Irradiance varies slowly over space and with the normal, like a temperature field, so it can be measured on a coarse grid of points and interpolated between them.

**What a probe stores.** Two small pictures of the whole sphere of directions, each unfolded flat by the **octahedral mapping** (project the sphere onto an octahedron, look down on it, fold the lower half out over the corners: one square, nearly even texels, simple borders):

- 8 x 8 texels of irradiance arriving round each direction;
- 16 x 16 texels of the distance to the nearest surface each way, as its mean and its mean square.

### How probes learn

Each frame a probe fires 256 rays by default (Low 32, Medium 64, High and Ultra 256; 16 to 1024 by hand) over a **Fibonacci spiral**, whose points spread evenly over a sphere. Sixteen frames take turns through one spiral sixteen times as long, each frame every sixteenth point from its own start, and the long spiral is turned randomly every sixteen frames so its gaps wander. It is the difference between sixteen people throwing darts at random and sixteen people each covering every sixteenth square of a grid.

Hardware ray tracing says what each ray hits:

- a miss brings back the sky;
- the back of a triangle brings back nothing (the probe is looking at the inside of something);
- a front face brings back its glow plus its matte colour times the light reaching it: the sun and the brightest few nearby lamps, each checked by a shadow ray, plus last frame's probe light there.

**Blending in.** The new answers do not replace the old ones; the probe keeps most of its value and mixes in a little, an exponential relaxation with a time constant (2 s for irradiance), like a capacitor charging through a resistor. The share kept is exp(−Δt/τ), so the light settles at the same speed at any frame rate. The blend averages the light itself: averaging a root of it (as the 2021 paper does) settles sparse light too dark, since a mean of roots is below the root of the mean (Jensen's inequality).

**Counting noise.** A probe's light is a Monte Carlo average, so it wobbles by about

  1/√(rays per frame × frames of memory)

Four times the rays, or four times the memory, halve it; dark rooms show it most, since their light comes from the few rays that find something bright. The price of a long memory is lag.

**Infinite bounces for the price of one.** A hit surface is itself lit by bounce light, which last frame's probes hold, so each hit adds its colour times that. Each frame's probes then carry one bounce more than the last, and a still scene sums E_direct (1 + ρ + ρ² + ...), which converges since the albedo ρ is below 1. A factor of 0.85 on the carried bounce (`bounceEnergy`) hides small leaks that repeated bounces would amplify.

### Memory, adapted

- **A ray budget.** Noise depends on rays per second times memory, not on how rays are split over frames, as a photograph's grain depends on total exposure, not on how many slices it is cut into. So above 60 probe updates a second (`probeUpdatesPerSecond`) each frame traces half or a quarter of the rays, with a little hysteresis so a frame rate hovering at a step does not flip back and forth.
- **An adaptive memory** (on by default). Each irradiance texel is a plain running mean since it last restarted, beside a short mean over the last fifth of a second and the measured scatter σ of one frame's estimate. A mean of n samples scatters by about σ/√n, so the short mean's spread is known; while it stays within eight such spreads of the stored light, differences are luck. Past that the light has really changed (a lamp switched on) and the texel restarts from the short mean: a significance test on every texel every frame. It cannot beat statistics: a dim patch seen by a fraction of a ray a frame changes only as fast as the long memory.
- **Scene events.** A game that knows a change happened (a switch flipped) sends a `ProbeEvent` for a region; texels there check for half a second whether their light really changed, and only then restart.
- **Fast lights: two memories.** A 2 s memory suits the sun and drags a swung flashlight's bounce for seconds. Light is additive, so lights a game marks `fast` (the flashlight) are kept in a second irradiance picture with a 0.1 s time constant, and the two are summed where read: two RC filters in parallel. Probes within reach of a fast light trace extra rays for it. With no fast light anywhere, that picture stays exactly zero.
- **Sharing between neighbours.** Probes a metre apart see nearly the same light, so averaging each with its six face neighbours pools their rays, like several thermometers in one room. It works only if their errors are independent, so each probe turns its rays its own way, and a neighbour counts only if the probe can see it. The averaged light goes into separate pictures everything reads, while each probe keeps learning its own, so the blur never compounds. The weight is 0 on Ultra and grows on lower presets, which trace fewer rays.

### Reading the probes at a pixel

A pixel sits in a cell with a probe at each of its 8 corners. Each corner's irradiance, read along the pixel's normal, is weighted by:

- **nearness**: trilinear weights, which sum to 1 and reproduce any linearly varying field;
- **side**: a probe behind the surface counts less, by ((cos + 1)/2)² + 0.2, soft so a small object still has probes that count;
- **visibility**, the leak-stopper. In the pixel's direction the probe knows the mean distance μ to the nearest surface and its variance σ² = (mean square) − μ². A point nearer than μ is visible; a farther one at distance r is visible with probability at most σ² / (σ² + (r − μ)²) by the one-sided Chebyshev inequality. A probe behind a wall sees the wall well before the point, the bound is tiny, and its light is all but switched off: the statistics of variance shadow maps.

The test point is first lifted off the surface, mostly towards the viewer (the **self-shadow bias**), since a surface is exactly where the probes' rays stop and the test is least sure. Small weights are crushed further (cubed below 0.2), because a faint wrong glow in a dark corner is what the eye catches first. The average is taken over square roots and squared at the end, so one bright probe pulls less.

Probes measure matte light. The shiny part reads the same probes along the mirror direction until the reflections ([Reflections and fog](reflections-and-fog.md)) replace it.

### Where the probes stand

A game places no probes, only one `ProbeCascades` component. The renderer keeps **cascades** of nested grids round the camera: six by default, the densest 0.5 m apart (31 x 17 x 31 probes), each next twice as far apart (21 x 13 x 21). It is mipmapping, or a multigrid solver, applied to space: bounce light is smooth and distant surfaces cover few pixels, so each extra grid doubles the reach for the same number of probes, and cost grows with the logarithm of the distance covered.

| Grid | Spacing | Full-strength reach |
|---|---|---|
| densest | 0.5 m | 6.1 m round the camera, 2.6 m up and down |
| fourth | 4 m | about 29 m |
| sixth | 16 m | about 116 m; past it, the flat sky ambient |

- **A fixed lattice.** Each grid sits on a lattice fixed in the world, offset a quarter metre so no probe lies on a whole or half metre, where levels put floors (a probe exactly on a surface starts its rays inside it).
- **A circular buffer.** When the camera crosses a cell, the plane of probes falling out behind is reused for the plane coming in ahead: grid place l lives in slot (l + scroll) mod count, and only scroll changes, so every other probe keeps its history. Neighbours are therefore found by grid place, never by slot number.
- **Smooth hand-over.** Each grid's weight fades round the camera itself, not its window, so it slides as you walk, and ends far enough inside the window that no pixel reads a plane that just arrived. New probes start from the coarser grid's light, counted as a few frames of history: a prior that fades as their own rays come in.

**Probes that move.** When a probe appears it fires 256 rays at the parts of the level marked never moving (`isStatic`). More than a quarter hitting back faces means it is inside something, and it moves out through the nearest face; a surface closer than a tenth of a cell pushes it straight away to a fifth of a cell. It never moves more than 0.4 of a cell, so a pixel still finds its eight probes by plain division; one buried too deep is switched off. (Stepping towards the farthest surface, as published, walked probes into doorways, where they lent the sunlit outdoors to the room.)

**Probes that sleep.** A probe is read only by surfaces in the eight cells round it. With no static surface there it sleeps and traces nothing; a moving object coming near wakes it, like a motion-sensing light. Standing in the light lab, only about a tenth of the probes are traced.

Frames without probes draw exactly as if the feature did not exist: probe sampling lives in separate pipeline variants chosen by a specialization constant, since even skipped code changed how the driver compiled the rest.

Exact details: [ddgi.md](../architecture/ddgi.md), [ddgi-cascades.md](../architecture/ddgi-cascades.md), [ddgi-memory.md](../architecture/ddgi-memory.md); the papers' formulas in `docs/ILLUMINATION.md`.
