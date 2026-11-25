# Virtual shadow rays (the sun's soft shadows in its pages)

Covers: `src/render/gpu/virtual_shadow_rays`, `assets/shaders/virtual_shadow_rays.glsl`

Soft shadows through the sun's virtual shadow pages (`virtual-shadows.md`) by shadow-map ray tracing (SMRT, Unreal's), phase 21 stage 4d-i. Research and choices: `docs/research/VIRTUALSHADOWMAPS.md` ("Soft shadows through the pages"). No ray tracing hardware: a ray is a line of texel reads through the pool, in the main pass's fragment shader.

## The idea

- From a point the sun is a disc of angular radius a (tan a = 0.00465). A ray towards its point u (|u| <= 1) climbs a height h along the light and moves h tan(a) u across it.
- A level maps (across, along) straight to (texels, depth), so a ray is a straight segment of texels and depths, marched in samples against the pool.
- The share of rays that get through is the share of the disc the point sees. Each ray finds its own blocker: a canopy over a canopy, sun flecks through gaps.

## The maths (`render/gpu/virtual_shadow_rays`, mirrored by `virtual_shadow_rays.glsl`)

| Function | What |
|---|---|
| `virtualShadowRayDisc` | a ray's point on the unit disc: a Fibonacci (Vogel) spiral, radius sqrt((i + 0.5) / n), turned by the pixel's noise |
| `virtualShadowSampleShare` | a sample's share of the ray's height: the last at 1, the rest ((k + jitter) / n)^2, nearer together near the point |
| `virtualShadowRayHeight` | the ray's height: its rim reaching `VIRTUAL_SHADOW_RAY_TEXELS` (12) across, no higher than the level's depth range (depth 1, the caster reach included) |
| `virtualShadowRaySample` | a sample's texel and depth |
| `virtualShadowFootprintFirst`, `virtualShadowFootprintIndex` | the first of the at most 2 x 2 pages a point's samples land on, and which of them a sample's page is |
| `virtualShadowMarchStep`, `virtualShadowRayBlocked` | the march, below |
| `virtualShadowRayVisibility` (C++ only) | the whole march over a callback's depths, as the main pass does it; what the tests march |

- **The clamp:** 12 texels is the cascades' widest penumbra (texels follow pixels, so about 6-12 pixels), as Unreal clamps its penumbra. Every sample lies on pages the point marked: the mark pass's reach (`VIRTUAL_SHADOW_FILTER_TEXELS`) is the rays' reach.
- **Taller casters:** the last sample sits at the ray's end, and a ray ending under a surface is blocked, so casters above the ray's height cast with the penumbra capped. Past the caster reach the far map's distant occluders answer, as before.

## The march

The pool holds only the first surface sunwards, so surfaces are thin shells (reversed depths: larger is sunwards).

| Sample | Rule |
|---|---|
| Under a stored surface, first time | it becomes the ray's surface; not blocked for being under it |
| Under the same surface carried on | within the bias plus the slope cap times the ray's rise: its slope updated |
| Under a higher surface hiding the ray's | the ray's surface carried on behind it (with a cap above 0), blocked when the ray rises to it; cap 0: the higher one taken instead |
| Under a lower surface | the ray's surface ended; the lower one taken |
| Out from under | blocked if the ray rose to its surface carried on here (through it, or through the gap between two samples); not if it slipped out sideways |
| After the last | blocked if hit or still under a surface |

- **A small leaf high up** leaves the point under it partly lit (the disc less the leaf), where a solid heightfield would black it out.
- **Overlapping blockers:** a ray that passed under a low plate and then under a higher one still meets the low one (Unreal's extrapolation behind occluders); with `extrapolateSlope` 0 it slips past and light leaks.
- **Slope cap:** `extrapolateSlope` (metres along the light a metre across) times tan(a), in stored depth per ray depth.
- **Bias:** the ray starts from the point lifted by 4c-iii's bias (`virtualShadowBiasDepth`), which is also the tolerance; open ground sloping to 60 degrees sees the whole sun.

## In the main pass (`scene_surface.glsl`)

`virtualShadowSun` takes `virtualShadowRayShadow(level, placed, entry, bias, tan a)` where it took one comparison.

- The point's four footprint entries read once (`virtualShadowTableIndex` of each page's slot; a page past the level's side counts as not drawn).
- Each sample's pool texel through its footprint entry (`virtualShadowFootprintDepth`); a sample on a page not drawn is skipped (4d-ii falls back to a coarser level).
- Noise: the pixel's interleaved gradient noise turns the rays and jitters the samples, shifted every frame (the record's frame below 1024), so the temporal pass averages the grain.
- **Cost:** four table reads and rays x samples pool reads a pixel on a drawn page (64 at 8 x 8); it grows with the screen's pixels on pages.

## Settings (`VirtualShadowSettings`, F5 Virtual shadows; the record's `soft` row)

| Field | Default | What |
|---|---|---|
| `rays` | 8 (Unreal's at Epic) | rays a point, at most 64; 0 is one hard comparison (4c-iii) |
| `samplesPerRay` | 8 (Unreal's) | samples a ray, 1 to 32, the last at its end |
| `extrapolateSlope` | 5 (Unreal's) | how steep a hidden surface may be carried on; 0 off |

Command line: `--set engine.render.virtualShadows.rays=N` (and `samplesPerRay`, `extrapolateSlope`).

## Tests

`virtual_shadow_rays_test` (the disc even and turned; shares climbing to 1, squared; a rim ray's end 12 texels across, the range's end capping it; every sample on its four footprint pages and the marked ones; each march rule; a plate's edge at 0.25, 1 and 2 m against the disc's exact share within 0.04; a leaf 0.4 of the penumbra wide leaving the point under it 0.84 lit; overlapping plates dark with the cap, leaking without; open ground flat and sloped to 60 degrees fully lit; no rays one comparison; missing pages skipped; settings, keys, the record's row, refusals); GPU: `virtual_shadow_rays_gpu_test` (disc, shares, heights, samples, footprint, the march's steps against the mirror).
