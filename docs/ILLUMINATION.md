# Illumination: research notes

Notes for building phase 6's global illumination. They record what the source papers actually say and do, the exact formulas and numbers from their reference code, and what that means for this engine. Read this before writing the DDGI tests and code. The plan itself stays in [ROADMAP.md](../ROADMAP.md); the finished explanation will go in [ENGINE_GUIDE.md](ENGINE_GUIDE.md).

Two papers by the same group define the method. Where they differ, this file says which one a rule comes from: **[2019]** for the original, **[2021]** for the production follow-up, which keeps the 2019 design and changes several parts of it.

## Sources

- **[2019] Majercik, Guertin, Nowrouzezahrai and McGuire, "Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields"**, *Journal of Computer Graphics Techniques* 8(2), 1-30. https://jcgt.org/published/0008/02/01/ (`paper.pdf`, or `paper-lowres.pdf` at 1 MB). Read in full.
  - Supplement: `https://jcgt.org/published/0008/02/01/supplement.zip` (620 MB, almost all video). `code/` holds the G3D reference: `sampleIrradianceField.pix` (shading from probes), `IrradianceField_updateIrradianceProbe.pix` (blending rays into probes), `IrradianceField_generateRandomRays.pix`, `IrradianceField.h/.cpp` (defaults and pass order). The paper says (section 5) the code, not the text, holds the exact details. Read in full.
- **[2021] Majercik, Marrs, Spjut and McGuire, "Scaling Probe-Based Real-Time Dynamic Global Illumination for Production"**, JCGT 10(2), 1-29. https://jcgt.org/published/0010/02/01/. What the authors changed while putting DDGI into the NVIDIA RTXGI SDK, Unity, Unreal Engine 4 and several games: one self-shadow bias instead of three, faster reaction to changes, probe relocation, probe states (off, sleeping, awake, vigilant), a compute-shader update, camera-tracking windows and nested volumes, and probe reuse for second-bounce reflections. It restates the whole algorithm, so it can be read on its own. Read in full.
  - Supplement: `https://jcgt.org/published/0010/02/01/supplement.zip` (640 MB, almost all video). `code/DDGIVolume/` holds `DDGIVolume.glsl` (the volume struct and the sampling function), `DDGIVolume_updateProbes.glc` (compute update), `DDGIVolume_computeProbeOffsets.glc` (relocation and initial states), `DDGIVolume_updateProbeFlags.glc` (waking), `DDGIVolume_copyProbeEdges.glc` (borders), `DDGIVolume_generateRays.glc`; `code/GIRenderer/GIRenderer_deferredShade.pix` shades with it. Read in full. Unlike 2019's, it holds no C++, so some defaults (the two hysteresis values, the energy preservation factor) are not stated anywhere.
- **McGuire, Mara, Nowrouzezahrai and Luebke (2017), "Real-Time Global Illumination using Precomputed Light Field Probes"**, I3D 2017. The precomputed predecessor: probe placement, octahedral probe textures and distance moments. Not read.
- **Cigolle et al. (2014), "A Survey of Efficient Representations for Independent Unit Vectors"**, JCGT 3(2). http://jcgt.org/published/0003/02/01/. The octahedral mapping.
- **Donnelly and Lauritzen (2006), "Variance Shadow Maps"**, I3D 2006. The Chebyshev visibility test (equation 5 there).
- **Marques et al. (2013), "Spherical Fibonacci Point Sets for Illumination Integrals"**, Computer Graphics Forum 32(8). The ray direction pattern.
- Further reading named by [2021], not read: Wang, Khiat, Kry and Nowrouzezahrai (2019), "Fast Non-Uniform Radiance Probe Placement and Tracing" (better placement than a grid); Dachsbacher and Stamminger (2005), "Reflective Shadow Maps" (less lag for a flashlight's bounce light).

Both papers are CC BY-ND 3.0. Write our own code from the formulas here rather than pasting the reference shaders.

## The idea in one paragraph

A diffuse surface's colour depends on the **irradiance** E arriving at it: the cosine-weighted integral of incoming radiance over the hemisphere around its normal, E(n) = ∫ L(ω) max(0, n·ω) dω. Irradiance changes slowly in space and with direction, so it can be measured at a coarse grid of points (probes) and interpolated. Classic irradiance probes do exactly that and leak: a probe on the bright side of a wall lights the dark side. DDGI fixes the leaks by storing, next to each probe's irradiance, the **distance to the nearest surface in every direction** (its mean and mean square), so a shaded point can tell whether a probe can actually see it. It keeps everything dynamic by re-tracing a few rays from every probe every frame and blending them into the stored values over time. [2021] adds the bookkeeping that makes it affordable: probes nudged out of walls, probes that are useless switched off or put to sleep, and dense probes only near the camera.

## What is stored per probe

Each probe stores two small spherical images, unfolded onto squares with the octahedral mapping (both papers settle on these sizes; [2021] chose them so one 64-thread GPU group updates one 8 x 8 probe exactly):

| Texture | Size per probe | What a texel holds | Reference format |
|---|---|---|---|
| Irradiance | 8 x 8 (+1 texel border = 10 x 10) | cosine-weighted mean radiance arriving from around that texel's direction ([2021]: raised to the power 1/5, see below) | [2019]: R11G11B10F in the text, RGB16F in the code. [2021]: RGB10A2 |
| Distance ("visibility") | 16 x 16 (+1 border = 18 x 18) | mean distance to the first hit and mean squared distance, both weighted by a sharp lobe around the texel's direction | RG16F |

- All probes are packed into one atlas per texture, like the shadow tiles. Each probe square has a 1-texel border so bilinear filtering across the edge of the unfolded square reads the right neighbours. The border copy rule ([2021] `copyProbeEdges`): a border texel copies the inner texel on the same edge **mirrored** about that edge's midpoint (the octahedron folds there), and each border corner copies the inner texel in the **opposite** corner. [2021] does it in a compute pass after each update.
- **Octahedral mapping** (Cigolle 2014): project a unit direction onto the octahedron |x| + |y| + |z| = 1 (divide by that sum), look at it from above, and fold the lower half outwards over the corners. The whole sphere becomes one square with less distortion than a cube map and only one face, so borders are easy. We need `octEncode`/`octDecode` in C++ (tested: round trip, every texel centre decodes to a unit vector, and the border copy rule matches decoding just outside the edge) and the same in GLSL.
- **Which irradiance value is stored.** The update averages ray radiance with weights max(0, n·ω) and divides by the sum of weights. Over evenly spread rays that estimates ∫L cos dω / ∫cos dω = E / π. So the stored value is E/π, not E ([2021] says the same thing: what the probe holds differs from true radiance by a constant factor).
- **Units in this engine.** `triangle.frag` computes `albedo * (ambient + Σ radiance * max(0, n·l)) + specular`, with no 1/π in the Lambert term. That makes our light `radiance` values the perpendicular irradiance divided by π, and physically the outgoing radiance of a Lambert surface is (albedo / π) E = albedo × (E / π). So the stored probe value **drops straight into the slot `ambient` has today**: `albedo * (probeIrradiance + diffuse)`. Both reference codes multiply by extra factors of 0.5π (the 2021 code applies one in sampling and another in shading) that belong to G3D's own conventions; do not copy them. Check ours with a "furnace test": a probe inside a uniform sky of radiance L stores L, and a white surface lit only by it shows L.
- **Formats here.** 8-bit formats flicker and lack range [2019]; 11-bit floats look the same as 16-bit floats with 45% less storage, but R11G11B10F has a slight green tint because it cannot store exact greys [2019]. [2021]'s RGB10A2 only works because its stored values are E^(1/5) and its scenes keep E/π at or below 1; this engine's lights are unbounded (the floodlights have intensity 40), so use a float format: `SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT` (or `R11G11B10_UFLOAT` if it supports storage writes) and `R16G16_FLOAT`. A compute shader that blends in place needs `SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_SIMULTANEOUS_READ_WRITE`; R11G11B10 storage writes are optional in Vulkan, so check `SDL_GPUTextureSupportsFormat`. Memory is small: 4096 probes cost about 3.3 MB of irradiance (RGBA16F) and 5.3 MB of distances.
- **Per-probe data** [2021]: a small integer texture (RGBA8 signed) per volume, one texel per probe: xyz = the probe's offset from its grid point (stored as a fraction of `probeOffsetLimit` × spacing), w = its state.

## Each frame

1. **Generate rays** for every probe that is awake or vigilant ([2021]; [2019] traces from every probe). n rays per probe ([2019] default 64; measured 32 to 256), in directions on a **spherical Fibonacci spiral** (point i of N: z = 1 - (2i + 1)/N, angle 2π · frac(i (φ - 1)), φ the golden ratio), all turned by **one random rotation per frame** (random axis, random angle) shared by every probe. A fixed set of 64 directions would always miss the same things; turning it each frame means the samples add up to the whole sphere over time. Rays start at the probe's (offset) position. Minimum hit distance: 0.08 in [2019]; **0 in [2021]**, because a probe cannot hit itself, and skipping near hits let rays escape through walls when probes sit close to them.
2. **Trace** every ray against the scene, storing each hit's position, normal and **which side it hit** (front or back face). A miss stores "no hit".
3. **Shade the hits** with the same shading routine the camera's pixels use: direct light from every light with shadows, plus indirect light read from the probes as they were last frame. The hit's "view direction" is the direction back to the probe. Each frame the probes gain one more bounce, so a still scene converges to infinite bounces for the price of one.
   - **Back-face hits** [2021]: radiance 0, and the distance is recorded **negative**. In the update, its distance becomes 0.2 × |d| ("shortened by 80%"), so the probe treats what lies behind as hidden and lights nothing through it. Setting the distance to 0 instead would drive the visibility weights to 0, which normalisation can then blow back up, and would skew probes that only see a few stray back faces from modelling quirks.
   - Glossy reflection at these secondary hits is not worth computing: the probe's cosine averaging blurs it away. Add the glossy part of the material to the matte colour instead [2019].
   - Energy loss on purpose [2019 code]: the indirect light read from probes at hits × `recursiveEnergyPreservation` 0.85, and every ray's radiance × 0.95 when blended. Tiny amounts of leaked light are very visible in dark rooms ("log perception"), and repeated bounces would otherwise amplify them. [2021]'s shading still multiplies by an `energyPreservation` uniform, value not given.
   - Ambient occlusion is not applied at the hits. Misses read the sky.
4. **Update the probes**, one GPU thread per texel ([2021] layout: the probe's rays loaded once into shared memory, then each thread loops over all of them; 3 times faster than [2019]'s pixel shader):
   - irradiance: weight w = max(0, texelDirection · rayDirection); accumulate w × radiance and w.
   - distance: w = max(0, texelDirection · rayDirection) ^ sharpness (50 in [2019]), a much narrower lobe so distances stay sharp at edges; accumulate w × d and w × d², with d clamped to `maxDistance` and a miss counting as `maxDistance` ([2019]: 1.5 × the length of one cell's diagonal; [2019] also pushes the hit point 0.01 out along its normal first).
   - divide by the summed weight (skip if it is about 0); [2021] then encodes irradiance as value^(1/5); then blend: **new = lerp(new, old, hysteresis)**, i.e. keep `hysteresis` of the old value, with the hysteresis adjusted per texel as in the next section. A probe that has just woken, just become vigilant or just been (re)created uses hysteresis 0, and a just-created one starts from 0.
   - copy the borders.

### Hysteresis, convergence and frame rate

[2019] uses one hysteresis, 0.85 to 0.98 (code default 0.98). Keeping a fraction α each frame is exponential relaxation, like an RC circuit, with time constant τ = -Δt / ln α. The papers ran at about 60 frames per second; this engine runs up to 240:

| α | τ at 60 fps | τ at 240 fps |
|---|---|---|
| 0.98 | 0.83 s | 0.21 s |
| 0.85 | 0.10 s | 0.026 s |

So a fixed α makes global illumination react 4 times faster (and 4 times noisier) at 240 fps. Set **time constants** in the settings and derive α = exp(-Δt / τ) each frame from `FrameDescription::frameSeconds`. Noise depends on rays per second of history, so slower GPUs can trace fewer rays per frame with a longer τ.

[2021] fixes the lag that [2019] accepted ("light flowing in and out") in four ways:

- **Separate hysteresis for irradiance and distance.** Visibility is kept as stable as possible (low hysteresis there made results unstable); the values are not given.
- **Perceptual encoding.** Store irradiance as E^(1/5) and blend in that space. Like a logarithm, it compresses bright values, so a drop from bright to dark moves the stored value most of the way at once; light-to-dark changes, the most visible lag, then fade at a perceptually even rate. 5 was best (lower converged slower; higher did not help). It also damps flicker from a single ray hitting a small bright spot. When sampling, raise the stored value to 5/2 (decoding to a square root) and blend probes in square-root space, as [2019] did, then square. A caveat of my own, not in the paper: averaging a power below 1 of noisy estimates is biased low (Jensen's inequality), so with few rays the converged light is slightly darker than the truth; worth measuring with the furnace test.
- **Per-texel thresholds** (irradiance only; too unstable for distances), on the encoded values: if the new value differs from the old by more than 0.25, lower the hysteresis by 0.15; by more than 0.8, use 0 (the scene has changed completely). The code takes the largest **signed** component difference before taking its absolute value, so a texel whose green drops a lot while red rises slightly is missed; use the largest absolute component difference instead.
- **Scene events** (game-signalled, applied to every probe, not just nearby ones; the paper suggests local would be better): small light change (a flashlight switched on) → irradiance hysteresis × 0.85 for 4 frames; large light change (time of day jump) → × 0.5 for 10 frames; large geometry change (a ceiling caves in) → irradiance × 0.5 for 10 frames and visibility × 0.5 for 7 frames. In this engine those frame counts become durations (about 1/15 s and 1/6 s at 60 fps).

Remaining limitation [2021]: small bright **moving** lights, flashlights in particular, still leave a visible trail in the bounce light (their video at 7:05). The demo's flashlight will show it. Reflective shadow maps handle that case better but only for one light type; DDGI treats all lights alike.

## Shading a point from the probes

For a point x with unit normal n and unit direction to the viewer v (for a probe-ray hit, v points back to the probe). These are the exact steps of the reference code; the papers describe them in prose.

1. **Self-shadow bias** [2021], one tunable value replacing [2019]'s normal bias and three statistical biases:
   **b = (0.2 n + 0.8 v) × 0.75 × minProbeSpacing × selfShadowBias**, selfShadowBias default 0.3. So with 1 m spacing the test point moves up to 0.225 m, mostly towards the viewer. Tests at the surface itself sit exactly on the boundary between seen and hidden, so the point is moved off it. Too little leaks dark and light; too much leaks light past corners (their figure 3). Raise it when fewer rays are traced (noisier distances).
   [2019] instead used x - p + (n + 3v) × 0.25 in scene units, unrelated to the probe spacing.
2. **Find the cage.** [2021] finds the grid cell and the trilinear fractions `alpha` = (x + b - basePosition) / spacing (0..1 per axis) from the **biased** point and the probes' grid positions (without their relocation offsets); [2019] used the unbiased point.
3. For each of the 8 corner probes p (at their relocated positions):
   - **Backface weight** (unbiased direction to the probe): w = ((n · dirToProbe + 1) / 2)² + 0.2. A wrap-shading curve, not a cut-off at the tangent plane: for small details inside a room, a hard cut-off can reject every probe that can see the point.
   - **Visibility (Chebyshev).** From the probe towards the biased point x + b, at distance r, read the distance texture: mean μ and mean square m₂, variance σ² = |m₂ - μ²|. If r ≤ μ the weight is 1; otherwise (σ² / (σ² + (r - μ)²))³. The fraction is Chebyshev's one-sided bound on the chance that a surface lies further than r, as in variance shadow maps; cubing sharpens it. [2021] then keeps it at least **0.05**, so when no probe can see the point there is still a fallback. Multiply into w.
   - w = max(w, 1e-6).
   - **Crush small weights:** if w < 0.2, w ← w³ / 0.2². Continuous at 0.2, it suppresses faint leaks the eye notices most in the dark.
   - **Trilinear weight**, after the crush so it is never crushed: w ×= Π over axes of (1 - alpha) or alpha, each factor at least 0.001 [2021].
   - Read the probe's irradiance in direction **n**, decode to square-root space, and accumulate w × value and w.
4. Divide by the summed weight, then square.

Optionally multiply by screen-space ambient occlusion on directly visible pixels [2019, 2021], because probes 1-2 m apart cannot see the small creases around a point. We have no SSAO; leave it out at first.

[2021]'s shader also shades a ray that was traced from a sleeping probe (should not happen) with a dull grey 0.1 rather than black, so a weight that slips through cannot cause a dark leak.

## Placing the grid, relocating and classifying probes

- Probes start on the corners of a regular 3D grid, a power of two per axis so indices are bit operations (an optimisation only). Spacing can differ per axis. **At least one full cage of 8 probes inside every room-like space**; 1-2 m spacing was enough for human-scale scenes [2019]. Probe count matters more than probe resolution [2019].
- **Relocation** [2021]. A grid ignores the level, so some probes land in walls or a hair from a surface, where they waste rays or cause leaks in acute corners. At start-up (and whenever a probe is created) trace distance-only rays and move each probe, at most `probeOffsetLimit` × spacing per axis (0.4 in the code; the text says at most 0.5, so the probe stays in its own cell and indexing still works), for **5 iterations** (a cap, so probes cannot oscillate through tangent back faces):
  - more than **25% of rays hit back faces** → it is inside something: move it through the closest back face, as far beyond it as the offset limit allows (solve for the largest scale of that direction that stays in the limit, minus 1 mm); if that scale is 1 or less it cannot get out, so do not move. Mark it **off**.
  - no front face seen at all (only back faces and sky) → **off**.
  - closest front face nearer than 0.05 → move it up to 0.2 towards the farthest front face, unless the farthest and closest directions agree (dot > 0.5: it only sees one surface), so it never passes through a surface.
  - a moved probe is marked uninitialised and evaluated again next iteration.
  - only static geometry counts. Moving probes around moving objects made results unstable.
- **States** [2021], stored per probe (code value in brackets):
  - **Off** [0]: still stuck in static geometry after relocation, or sees no front face. Never traced or updated.
  - **Sleeping** [1]: no geometry within about one probe spacing, so nothing samples it. Not traced. Light changes and camera distance do not matter: a probe is needed if and only if it shades (or is about to shade) a surface.
  - **Vigilant** [5] (**Newly vigilant** [4] for its first update): a static surface lies within the shading range (code: any front-face hit inside a box of one spacing plus 2 × self-shadow bias around the probe). Always traced and updated, even off screen, because it carries bounce light to other surfaces.
  - **Awake** [3] (**Newly awake** [2]): a sleeping probe inside the box of a **moving object** grown by one cell plus the self-shadow bias. Re-checked every frame: probes that are neither off nor vigilant go back to sleeping when no box contains them.
  - **Uninitialised** [6]: new, or just moved, or just scrolled in by a tracking window; data reset to 0.
  - "Newly" states take hysteresis 0 for their first update (optionally with many extra rays to converge in one frame), then become their plain state.
  - Saving: 30-50% of probe tracing and updating time in their scenes, which can be spent on more rays per probe instead.
  - For fog or other participating media later, probes inside the medium would have to stay awake, since empty space then gets shaded too.
- The [2021] text and code differ in detail here (see the table at the end); follow the code.

## Large worlds: tracking windows and nested volumes [2021]

- **Tracking window.** A grid over a whole level would need tens of thousands of probes, so a volume follows the camera (or any object). It is a 3D circular buffer: when the camera moves more than one probe spacing from the volume's centre along an axis, the plane of probes furthest behind it is reused as a new plane in front ("leapfrogs"), by changing a per-axis phase offset rather than copying data. The new plane is uninitialised: relocated and classified again, hysteresis 0.
- **Several volumes** at different spacings (a dense one at the camera inside coarser ones, like geometry clipmaps; or a dense one around an important object). Rays for all volumes can be packed and traced in one batch.
- **Blending volumes.** Each volume's weight at a point is 1 inside and falls linearly to 0 across its outermost cell on each axis (a product over the axes). Sample volumes from densest to sparsest, add each one's weight × (1 - weight so far), and stop once the total reaches 1. For a camera-locked volume, shrink the blend region by one cell and centre it on the camera, otherwise a leapfrogging plane makes points jump from the coarse to the dense volume in one frame.
- Finding which volumes contain a pixel: loop over them (cheap below about 10), or for pure cascades (axis-aligned, nested regularly) compute it directly.

## Reflections from probes [2021]

Second and later bounces of glossy reflections can sample the irradiance probes in the reflected direction instead of tracing further rays: a reflection forced to maximum roughness. Not needed until phase 14's ray-traced reflections exist; noted so the probe data structure is not designed in a way that rules it out.

## Costs measured

- [2019], RTX 2080 Ti with OptiX hardware ray tracing, 32 x 8 x 32 probes (8192), 64 rays each (524,288 rays a frame), every probe every frame: ray generation 0.1 ms, tracing 0.8 ms, shading the hits 0.4 ms, probe update 0.7 ms, sampling for the screen 0.5 ms; 2.6 ms in all. About 1.5-1.65 billion rays a second from 32 to 256 rays per probe.
- [2021]: sleeping saves 30-50%; the compute update is 3 times faster than the pixel-shader one. Controlling rays per probe and probes per volume was enough to hit their budgets; a per-frame ray budget is left as future work.

This engine traces in compute shaders against a simplified scene without RT cores (the roadmap's signed distance field or voxel grid), so expect tracing to be the expensive part, and lean on [2021]'s savings: states, a camera-tracking window, fewer rays with a longer time constant. Ray tracing and global illumination quality must stay optional and scalable, so spacing, volume size, rays per probe and the time constants belong in a settings struct with defaults tuned for typical GPUs, not an RTX 5080.

## What this means for the engine

- **Build the 2021 version of each part where it differs**: the self-shadow bias, the 1/5 encoding with per-texel thresholds, separate hysteresis for distances, back-face hits shortened, no minimum ray distance, the cage found from the biased point, the 0.05 visibility floor, the compute update with borders copied in compute. Add relocation and states once the basics work; tracking windows and nested volumes last.
- **Order of work.** HDR first (probe values and bounced light go above 1), then compute pipelines, then physically based materials before tuning, because bounce light depends on albedo.
- **Ray tracing must report back faces**, since both relocation and leak prevention depend on it. A signed distance field gives that for free (a ray that starts inside sees negative distance); a voxel grid needs inside/outside marked.
- **Shading the ray hits needs the direct lighting in a compute shader**: the light uniforms and the shadow atlas, readable outside `triangle.frag`. Both papers use a deferred renderer; we are a forward renderer, so share the lighting functions between the fragment and compute shaders (GLSL `#include`, which glslc already supports here). The roadmap's caveat stands: the shadow atlas only holds lights and cube faces relevant to the camera, while vigilant probes off screen still need direct light, so they need their own lights kept or go unshadowed.
- **One shading function for both uses**, in one GLSL file both include, with its weight maths mirrored in C++ for tests, like `render/gpu/soft_shadow`.
- **Moving objects**: they only affect bounce light if they are in what the rays trace against, so the simplified scene must include dynamic entities. Their boxes also wake probes; entities with a `PreviousTransform` are exactly the ones that move continuously, and `boundsByMesh` / `Collider` give their boxes.
- **Static geometry for relocation**: the static level (`StaticLevel`) is the natural input, since relocation and the vigilant/off classification consider static geometry only.
- **Per frame, not per tick.** Probe updates run with rendering and use the frame's Δt; nothing about them belongs in the fixed-step simulation. The scene-event speed-ups ("a light switched on") are commands from the game to the renderer, like the light priority hints.
- **MOBA and FPS fit the tracking window**: one camera-locked dense volume, plus a coarse volume over the whole map.
- **Things to test on the CPU first** (our usual mirror-and-test style): octahedral encode/decode and the border copy rule; Fibonacci directions (unit length, evenly spread: their mean is near zero); the irradiance update (a uniform sky gives a uniform value; one bright direction gives a cosine lobe); back-face distance shortening; the 1/5 encoding round trip and the change thresholds; hysteresis as a time constant (after τ seconds, 1/e of a step remains, at any frame rate); the self-shadow bias; the Chebyshev weight (1 up to the mean, falling behind it, never below 0.05); the cage weights (trilinear alone reproduces linear functions, all weights positive); relocation (a probe inside a box moves out or turns off; a probe near one face moves away from it; never beyond the offset limit); the state transitions; tracking-window leapfrogging (indices wrap, only the new plane resets); volume blend weights (sum to 1 inside the dense volume, smooth across its edge); the furnace test.
- **Debug view** (already planned): draw each probe as a small sphere shaded by its stored irradiance, with off, sleeping and vigilant probes marked by colour, as both papers' figures do.

## Paper text versus reference code

| Topic | Paper text | Reference code |
|---|---|---|
| Irradiance format | [2019] R11G11B10F | [2019] RGB16F; [2021] RGB10A2 |
| Backface weight | [2019] soft cut-off at the tangent plane | wrap shading ((cos + 1)/2)² + 0.2 |
| Low-light suppression | [2019] lower very low irradiance values | crush weights below 0.2 (cubic) |
| Texel update threshold | [2019] 0.001 | 1e-6 |
| Hysteresis | [2019] 0.85-0.98 | [2019] 0.98; [2021] values not given |
| Energy loss per bounce | not mentioned | [2019] 0.95 on ray radiance, 0.85 on recursive indirect; [2021] a uniform, value not given |
| Blend space | not mentioned in [2019] | [2019] square roots; [2021] stored as E^(1/5), blended as square roots |
| Equation (1) | unnormalised sum over rays | normalised by the summed weights |
| Change threshold | [2021] "magnitude above 25% / 80%" | largest signed component difference (misses some drops) |
| Relocation inside geometry | [2021] ramp a scale factor down from 2 in 100 steps | solve for the largest move inside the offset limit |
| Offset limit | [2021] half the spacing | 0.4 of the spacing |
| Vigilant test | [2021] closest front face within one probe spacing | any front-face hit inside a box of one spacing plus 2 × self-shadow bias, or one closer than 0.05 (comment says 2 cm) |
