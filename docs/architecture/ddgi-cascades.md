# Light probe cascades and relocation

Covers: `src/scene/probe_volume.h`, `src/render/gpu/probe_cascades`, `src/render/gpu/probe_relocation`, `assets/shaders/probe_cascades.glsl`, `assets/shaders/probe_cascade_holding.glsl`, `assets/shaders/probe_relocation.glsl`, `assets/shaders/ddgi_relocate.comp`, `assets/shaders/ddgi_seed.comp`, `src/scene/probe_volume`

Where the light probes stand, which move, which are traced, and how new ones start. On hold with the rest of the probes: `ddgi.md`.

## Placing probes

A game places one `ProbeCascades` (`scene/probe_volume.h`: `followCamera` true, else `centre`); `collectLighting` puts the first in `FrameLighting::probeCascades`. The renderer keeps nested grids round it, so no level places or tunes probes.

**Layout** (`probeCascadeLayout(settings)`, `ProbeCascadeLayout`):

| Setting | Default | Meaning |
|---|---|---|
| `probeCascades` | 6 | How many (at most `DDGI_MAX_CASCADES` 6) |
| `probeSpacing` | 0.5 m | Densest spacing; each next cascade doubles it (`probeCascadeSpacing`) |
| `densestProbesAcross`, `densestProbesHigh` | 31, 17 | Densest cascade, level axes and up |
| `cascadeProbesAcross`, `cascadeProbesHigh` | 21, 13 | Every other cascade |

- At least 2 a side; the up axis gives way until a layer fits the atlas width and one dispatch, then as many cascades as fit.
- Presets: High and Ultra the defaults; Medium 5 cascades, densest 25 x 15 x 25, others 17 x 11 x 17; Low 4 from 1 m, densest 19 x 11 x 19, others 15 x 9 x 15.
- Changing any of these rebuilds the atlases (a hitch).

**Windows** (`probeCascadesAround(centre, layout, bias)`, densest first, probes numbered cascade after cascade from `ProbeCascade::firstProbe`; each from `probeWindow`):

- On its spacing's lattice, offset from the world origin by `probeLatticeOffset` (`DDGI_LATTICE_OFFSET` 0.5 of the densest spacing, the same metres for every cascade, so they nest). The offset keeps probes off whole and half metres, where levels put floors and walls: a probe exactly on a surface starts its rays on it.
- `start` = lattice point nearest the centre less (count - 1) / 2 (`probeWindowStart`): the middle probe within half a cell of the centre.
- `volume`: a `ProbeVolume`, origin offset + start x spacing.

**Scrolling** (`ProbeVolume::scroll`, `ProbeScroll`): storage is a circular buffer.

- Grid place l lives in slot (l + scroll) mod count (`probeSlot`, `probeGridCoord`, `probePosition(volume, slot)`; `probeLatticeCoord(cascade, slot)` = start + place).
- A one-cell move keeps every lattice point's slot; only the plane brought in changes (`scrolledProbes(before, after)`; all of them when size or spacing differ).
- Corners, neighbours and event regions are found by grid place, never by slot.

**Weight** (`probeCascadeWeight`): the product over axes of clamp((reach - |point - centre|) / fade, 0, 1).

- Centred on the followed point, not the window, so it moves smoothly and nothing pops when a plane jumps.
- `probeCascadeReach(count, spacing, biasReach)` = (floor((count - 1) / 2) - 0.5 - `DDGI_CASCADE_EDGE` (1)) x spacing - the bias reach; fade `DDGI_CASCADE_FADE` (1) cell. So the cell round any lit point (from its biased point) never touches an outer plane a scroll may just have brought in.
- Full-weight reach at the defaults: densest 6.1 m round, 2.6 m up and down; next 7.3 m and 3.4 m; fourth 29 m and 13 m; fifth and sixth (8 m and 16 m apart) 58 m and 116 m. Beyond: the flat sky ambient.
- `fixedProbeCascade(volume)`: a volume that stays put, full weight out to `DDGI_VOLUME_MARGIN` (half a cell) past its outer probes, fading over `DDGI_VOLUME_FADE`; for tests placing probes by hand.

**Packed** (`packProbeCascades(cascades, cameraOrigin)`, `GpuProbeCascade` 112 bytes, one a cascade, set 1 binding 20 and the update and share passes): origin from the camera with its longest recorded distance, spacing, scroll and first probe, weight centre, reach and fade, counts. Mirrored in `probe_cascades.glsl`; tested in `probe_cascades_test` and `ddgi_gpu_test`.

## Static geometry

- Games mark the level with `ModelRenderer::isStatic` (→ `DrawItem::isStatic`); static shadow casters carry `RAY_MASK_STATIC` too (`ray-tracing.md`). Unmarked draws count as moving.

## Relocation

A grid ignores the level: some probes land in walls (seeing back faces, lending darkness), some a hair from a surface. `ddgi_relocate.comp`; `relocateProbe` on the CPU; mirrored in `probe_relocation.glsl`; tested in `probe_relocation_test`, `probe_relocation_gpu_test`.

- **Asked for:** every probe after new cascades, a `GeometryChange` event or switching `probeRelocation`; else the planes a scroll brought in (`relocationRequests`, `ProbeRelocationRequest`). One workgroup of `DDGI_RELOCATION_GROUP_SIZE` (64) a probe; push constants `GpuProbeRelocate`.
- **Rays:** `DDGI_RELOCATION_RAYS` (256), distance only, plain Fibonacci spiral (no rotation: repeatable), static mask; hits nearer than `DDGI_RELOCATION_MIN_DISTANCE` (1 mm) skipped, so a probe on a surface or a seam between solids sees the real inside.
- **Tally** (`ProbeRayTally`, `tallyProbeRay`): back and front faces, nearest back face, nearest and farthest front faces, how far the nearest front-face hit lies beyond the probe's cells.
- **Steps** (`relocateProbeStep`, up to `DDGI_RELOCATION_ITERATIONS` 5, stopping when still or stuck):

| Case | Move |
|---|---|
| Inside (over `DDGI_INSIDE_BACK_FACE_SHARE`, 25%, of rays hit back faces) | Out through the nearest back face, scaled to the offset limit less `DDGI_ESCAPE_MARGIN` (`probeEscapeScale`); a scale of 1 or less cannot reach it: stuck |
| Front face nearer than `DDGI_MIN_FRONT_FACE_SHARE` (0.1) of the smallest spacing | Straight away from it to `DDGI_FRONT_FACE_NUDGE` (0.2) of that spacing, clamped |

- Offset limit `DDGI_OFFSET_LIMIT`: 0.4 of the spacing per axis, so the probe stays in its cell and the grid still finds it.
- Away from the nearest front face, not towards the farthest as the 2021 reference does: in a room the farthest lies out through a door, and probes walked into doorways.

**Records** (`finishProbeRelocation`, `GpuProbeRelocation` 32 bytes: offset, off (stuck or inside), nearest surface beyond the cells or the largest float):

- Written to the frame in flight's read-back memory, read when that frame in flight comes round again (`collectRelocation`: its fence has passed, so nothing waits). `collectAllRelocations` after the frames-in-flight count changes.
- Kept only if the probe is still at the lattice point asked for (`keepProbeRelocations`).
- A scrolled-in probe waits pending (`pendingProbeRelocation`, `probeRelocationPending`): off, never traced or read, until its record.

## Probe states

`ProbeState`, `DDGI_PROBE_*`; `classifyProbe`, `buildProbeStates`, per cascade through `buildCascadeStates`.

| State | Value | Meaning | Traced |
|---|---|---|---|
| Off | 0 | Buried or pending: never traced or read | No |
| Sleeping | 1 | Nearest static surface beyond its cells grown by the bias reach: no surface reads it (also a probe seeing nothing) | No |
| Awake | 2 | Sleeping, but within a mover's bounds (`movingBounds`) grown by the bias reach and `wakeCells` (1) cells | Yes |
| Vigilant | 3 | A static surface within that reach | Yes |

- Bias reach: `probeBiasReach(spacing, bias)` = 0.75 x smallest spacing x bias, the longest the self-shadow bias gets.
- `GpuProbeState` (16 bytes: offset, state) per probe per frame, set 1 binding 16; `probeTraced`.
- No records yet, or `probeStates` off: every probe vigilant. `probeRelocation` off: no offsets (the pass runs with 0 steps, classifying only).
- **Restarts:** a probe traced now but not before, moved, or scrolled in and traced at once (`restartedProbes`) restarts outright (`advanceProbeRestarts(..., restarted, seeded)`): nothing kept on its first frame, event check threshold 0 (`ddgi-memory.md`).
- **Readout:** `countProbeStates` into `RenderStats::probes`; F4 line `PROBES n  TRACED n  AWAKE n  ASLEEP n  OFF n  MOVED n` (`probeStatsText`).

## The traced list

`chooseTracedProbes(states, capacity, turn)`: every awake and vigilant probe, in order, up to `DDGI_MAX_TRACED_PROBES` (8,192).

- Past the cap (before the first records, or states off): a window of 8,192 moving on by that many each frame (`updatedFrames` the turn), so all take turns, none twice a frame.
- The ray buffers hold rays only for listed probes (slot in the list x stride + ray): `rayCapacity` x `tracedCapacity`, grown after `waitIdle`. At 24 bytes a ray: 256 rays 50 MB, 1024 rays 201 MB. Near-fast-light flags stay per probe.

## Seeds

New probes start from the coarser cascades' light where they stand, instead of their first frame's rays alone (`seedProbes`, on).

- `chooseProbeSeeds(restarted, cascades)`: all restarted but the coarsest cascade's, at most `DDGI_MAX_SEEDS` (2048) a frame, each a seed slot; only once the atlases are not fresh and have been updated more than `DDGI_SEED_FRAMES` frames.
- `ddgi_seed.comp`: one group per seed, one invocation per texel; the trace's sets 0-2 plus set 3 (seeded probes, seeds: 8 x 8 texels of two `vec4`s). Reads `probeAmbient` from the next cascade on, with normal, lookup and view all the texel's direction.
- The update starts the texels from it (`seededProbeTexel`: stored and window mean the seed, history -`DDGI_SEED_FRAMES` (4), noise 0; the fast atlas its seed); `GpuProbeRestart::seed` makes the restart count from there, so the seed fades as rays come in.
- Distances are never seeded.
