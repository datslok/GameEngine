# Virtual shadow rays (the sun's soft shadows in its pages)

Covers: `src/render/gpu/virtual_shadow_rays`, `assets/shaders/virtual_shadow_rays.glsl`

Soft shadows through the sun's virtual shadow pages (`virtual-shadows.md`) by shadow-map ray tracing (SMRT, Unreal's), phase 21 stages 4d-i and 4d-ii. Research and choices: `docs/research/VIRTUALSHADOWMAPS.md` ("Soft shadows through the pages", "Wide penumbrae"). No ray tracing hardware: a ray is a line of texel reads through the pool, in the main pass's fragment shader.

## The idea

- From a point the sun is a disc of angular radius a (tan a = 0.00465). A ray towards its point u (|u| <= 1) climbs a height h along the light and moves h tan(a) u across it.
- A level maps (across, along) straight to (texels, depth), so a ray is a straight segment of texels and depths, marched in samples against the pool.
- The share of rays that get through is the share of the disc the point sees. Each ray finds its own blocker: a canopy over a canopy, sun flecks through gaps.
- **Wide penumbrae through coarser levels:** a sample reads the first level, from the point's own, where it lies within `VIRTUAL_SHADOW_RAY_TEXELS` (12) of the point across. A ray climbing to a caster far above reads texels as coarse as its spread: a rock pillar's edge 100 m up casts its 47 cm penumbra, where one level's 12 texels near the camera allow about 5 cm.

## The maths (`render/gpu/virtual_shadow_rays`, mirrored by `virtual_shadow_rays.glsl`)

| Function | What |
|---|---|
| `virtualShadowRayDisc` | a ray's point on the unit disc: a Fibonacci (Vogel) spiral, radius sqrt((i + 0.5) / n), turned by the pixel's noise |
| `virtualShadowSampleShare` | a sample's share of the ray's height: the last at 1, the rest ((k + jitter) / n)^p, p = max(2, levels up), by multiplying: half the samples stay within the point's own level's reach however long the ray, so contact shadows keep theirs |
| `virtualShadowRayHeight` | the ray's height: its rim reaching 12 x 2^(levels up) of the point's level's texels, no higher than the level's depth range (depth 1, the caster reach included) |
| `virtualShadowSampleLevel` | how many levels coarser a sample reads: the first where its offset is within 12 texels (by doubling, exact), at most the levels up |
| `virtualShadowStoredHeight` | a level's stored depth as a height above the point in metres (each level has its own depth scale) |
| `virtualShadowRaySample` | a sample's texel and depth |
| `virtualShadowFootprintFirst`, `virtualShadowFootprintIndex` | the first of the at most 2 x 2 pages a point's samples land on in its own level, and which of them a sample's page is |
| `virtualShadowMarchStep`, `virtualShadowCrossingDepth`, `virtualShadowMarchCheck`, `virtualShadowRayBlocked` | the march, below |
| `virtualShadowRayVisibility` (C++ only) | the whole march over a callback's levels and depths, as the main pass does it; what the tests march |

- **The clamp:** a ray reaches at most `rayLevels` (4) levels up, so the widest penumbra is 12 x 2^4 = 192 of the point's level's texels (about 0.8 m a few metres from the camera: a caster 170 m up), as Unreal clamps its penumbra. With `rayLevels` 0, 12 texels (the cascades' widest).
- **Marked pages:** every sample lies within 12 texels of the point in the level it reads; the mark pass asks for that square round each pixel in its own level and its `rayLevels` coarser ones (`virtual-shadows.md`).
- **Missing pages:** a sample whose page is not drawn reads the next coarser drawn level; past all levels it is skipped. A point whose own page is not drawn takes the first coarser drawn level as its own; none: the cascades.
- **Taller casters:** the last sample sits at the ray's end, and a ray ending under a surface is blocked, so casters above the ray's height cast with the penumbra capped. Past the caster reach the far map's distant occluders answer, as before.

## The march

The pool holds only the first surface sunwards, so surfaces are thin shells. The march works in heights above the point (metres; larger is sunwards), so samples of different levels march together.

| Sample | Rule |
|---|---|
| Under a stored surface, first time | it becomes the ray's surface; not blocked for being under it |
| Under the same surface carried on | within the tolerance plus the slope cap times the ray's rise: its slope updated |
| Under a higher surface hiding the ray's | the ray's surface carried on behind it (with a cap above 0), blocked when the ray rises to it; cap 0: the higher one taken instead |
| Under a lower surface | the ray's surface ended; the lower one taken |
| Out from under, risen to its surface's height | crossed it between two samples: read the crossing |
| Out in the open past a surface higher than the ray was at the last sample | crossed that height between two samples: read the crossing |
| After the last | blocked if hit or still under a surface |

- **The crossing read** (as screen-space ray marches refine a hit): one more read where the ray was at the crossed height, between the two samples; blocked if the surface reaches that height there (through it), not if it does not (out beside it, or past its edge); no page there: blocked. One read a crossing, not a sample. Without it the penumbra came out narrow (too dark inside, too light outside), worse the longer the ray: up to 0.06 off the disc's share; with it under 0.006.
- **A small leaf high up** leaves the point under it partly lit (the disc less the leaf), where a solid heightfield would black it out.
- **Overlapping blockers:** a ray that passed under a low plate and then under a higher one still meets the low one (Unreal's extrapolation behind occluders); with `extrapolateSlope` 0 it slips past and light leaks.
- **Slope cap:** `extrapolateSlope` (metres along the light a metre across) times tan(a), surface height per ray height.
- **Bias:** the ray starts the point's level's bias (4c-iii's `sunShadowBias` in its texels) above the point; each sample's tolerance is its own level's bias. Open ground sloping to 60 degrees sees the whole sun.

## In the main pass (`scene_surface.glsl`)

`virtualShadowSun` takes `virtualShadowRayShadow(base, lightPoint, nDotL, tan a)`: base the point's level, or the first coarser one with its page drawn.

- The point's four footprint entries in its own level read once; a sample there takes its entry from them, a coarser level's from the table (`virtualShadowEntryAt`; a page past the level's side counts as not drawn).
- `virtualShadowSurfaceAt` finds a sample's surface: from its level up to the first drawn, the pool's depth (`virtualShadowPoolDepth`, shared with the debug view) as a height, that level's bias as the tolerance.
- Noise: the pixel's interleaved gradient noise turns the rays and jitters the samples, shifted every frame (the record's frame below 1024), so the temporal pass averages the grain.
- **Cost:** a table read and a pool read a sample (four table reads once for the point's own level), one more of each a crossing; 64 samples at 8 x 8. It grows with the screen's pixels on pages.

## Settings (`VirtualShadowSettings`, F5 Virtual shadows; the record's `soft` row)

| Field | Default | What |
|---|---|---|
| `rays` | 8 (Unreal's at Epic) | rays a point, at most 64; 0 is one hard comparison (4c-iii) |
| `samplesPerRay` | 8 (Unreal's) | samples a ray, 1 to 32, the last at its end |
| `extrapolateSlope` | 5 (Unreal's) | how steep a hidden surface may be carried on; 0 off |
| `rayLevels` | 4 | levels a ray may climb above the point's, 0 to 8 (also marked by each pixel; 0 without rays) |

Command line: `--set engine.render.virtualShadows.rays=N` (and `samplesPerRay`, `extrapolateSlope`, `rayLevels`).

## Tests

`virtual_shadow_rays_test`: the disc even and turned; shares climbing to 1, squared, half within a sixteenth four levels up; a rim ray's end 12 x 2^k texels across, the range's end capping it; sample levels at each reach and a float step past; stored heights; every sample within 12 texels in its level, on its four footprint pages in its own; each march rule and crossing read; levels each with their own texel, origin, ground depth and depth scale: a plate's edge at 0.25, 1 and 2 m against the disc's exact share within 0.02, one 100 m up near the camera through coarser levels within 0.02 (sharp with `rayLevels` 0), and both with levels missing; a leaf 0.4 of the penumbra wide leaving the point under it 0.84 lit; overlapping plates dark with the cap, leaking without; open ground flat and sloped to 60 degrees fully lit; no rays one comparison; nothing drawn: lit; settings, keys, the record's row (no levels up without rays), refusals. GPU: `virtual_shadow_rays_gpu_test` (disc, shares, heights, samples, footprint, sample levels, stored heights, the march's steps and crossing reads against the mirror).
