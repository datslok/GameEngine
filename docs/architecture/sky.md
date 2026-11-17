# Sky and atmosphere

Covers: `src/scene/atmosphere`, `src/render/gpu/atmosphere`, `src/render/gpu/atmosphere_shared.h`, `src/render/gpu/stars`, `src/render/vulkan/vulkan_atmosphere`, `src/render/vulkan/vulkan_stars`, `assets/shaders/atmosphere.glsl`, `assets/shaders/atmosphere_tables.glsl`, `assets/shaders/scene_sky.glsl`, `assets/shaders/sky.frag`, `assets/shaders/stars.glsl`, `assets/shaders/stars.vert`, `assets/shaders/stars.frag`, `assets/shaders/atmosphere_ambient.comp`, `assets/shaders/atmosphere_multiple_scattering.comp`, `assets/shaders/atmosphere_sky_view.comp`, `assets/shaders/atmosphere_transmittance.comp`

The sky drawn behind everything, the air every directional light crosses, the moon and stars, and the flat ambient light. Hillaire 2020 ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique"). The physics for a reader: `docs/ENGINE_GUIDE.md`, Light through the air. Shader mirror rules: `.claude/rules/shader-mirror.md`. Indoor dimming of the flat ambient: `sky-visibility.md`.

## Scene side

| Module | What |
|---|---|
| `scene/atmosphere.h` | `Atmosphere` component: planet and air radii (km), Rayleigh, Mie and ozone coefficients (per km), ground albedo; Earth's by default. `groundAltitude`: the world's y = 0 above the surface. `sunDiscScale`, `moonDiscScale`: drawn disc size against the real one (set by the game, not in `visit`) |

- Where the sun, moon and stars stand, the star catalogue and sky discs: `sun-moon-stars.md`.
- `FrameLighting::atmosphere` and `FrameLighting::stars`: the first of each component in the world.
- The moon is a `DirectionalLight` with `reflectsSun`. Games place it and the sun themselves (the light lab).

## API-free maths

`render/gpu/atmosphere`, numbers in `atmosphere_shared.h`, mirrored by `atmosphere.glsl`.

**Frame:** the planet's, km, y up; camera at (0, `atmosphereViewRadius`, 0): ground altitude + camera height, at least `ATMOSPHERE_MIN_ALTITUDE` (1 m) so the horizon stays defined.

| Function | Role |
|---|---|
| `atmosphereMedium`, `rayleighPhase`, `henyeyGreensteinPhase` | Scattering and extinction at an altitude; phase functions |
| `rayDistanceToSphere` | Written from (r − R)(r + R) and the near root from the far, since a float at the planet's radius rounds to half a metre |
| `marchSample`, `atmosphereMarchLength` | Steps growing as the square of their index, sampled at midpoints (`ATMOSPHERE_SAMPLE_OFFSET` 0.5); march to the ground or the top |
| `atmosphereTransmittance` | 40 steps; 0 below the horizon, 1 above the air |
| `scatteringMarchStep` | Energy-conserving step (source − source·T) / extinction, `oneMinusExp`'s series for thin air |
| `skyStepSource`, `groundRadiance` | A step's gathered light (takes an illuminance, so sun and moon share it); the ground as matte albedo |
| `multipleScatteringTexel` | 64 Fibonacci directions × 20 steps, isotropic; mean light / (1 − mean share scattered again): every order as a geometric series |
| `skyRadiance` | The CPU's sky view texel (`AtmosphereLookups`: tables or `exactAtmosphereLookups`), the sunlit ground included, no disc |
| `skyAmbient` | Flat ambient of a sky: 4/N Σ sky·up over 256 Fibonacci directions above |
| `throughAtmosphere` | A directional light times the transmittance towards it |
| `atmosphereSkyLights` | Which lights light the sky: sun = brightest self-lit, moon = brightest `reflectsSun` (above-air value) |
| `lommelSeeliger`, `lommelSeeligerDiscShare` | Moon disc shading 2 μ0 / (μ0 + μ) (full moon flat edge to centre); its disc mean 1 − sin(a/2) tan(a/2) ln cot(a/4) |
| `packAtmosphere` | `GpuAtmosphere` (192 bytes, std430) from `Atmosphere` and `AtmosphereFrame` |

**Table coordinates** (texel centres on the ends):

| Table | Size | Axes |
|---|---|---|
| Transmittance | 256 × 64 | Bruneton and Neyret's (`transmittanceLutUv`/`Ray`) |
| Multiple scattering | 32 × 32 | sun cosine across, altitude down |
| Sky view | 192 × 108 per sky, 2 skies stacked (192 × 216) | zenith angle down, squared towards the horizon on both sides; bearing from the light across. `skyViewLookup`, `skyViewAtlasUv` keeps each sky's texel centres apart so filtering never mixes them |

## Two skies

- The sky view table holds one sky per light (`ATMOSPHERE_SKY_VIEW_LIGHTS`: sun, then moon). Scattering is linear in the light, so the moon's sky is the same march with its direction and illuminance (`atmosphereLightDirection`, `atmosphereLightIlluminance`); a light giving nothing skips its march.
- `skyFromTable` sums both, each at its own light's bearing, so the sky pass, the flat ambient and escaping probe rays all gain moonlight.

## VulkanAtmosphere

Three RGBA16F storage images (the tables) and a one-`vec4` GPU buffer (the flat ambient), shared by the frames in flight; barriers put this frame's writes after last frame's reads.

`update` (GPU pass `Atmosphere`, `SKY` in F4; after the ray scene, before the probes):

1. `atmosphere_transmittance.comp`, `atmosphere_multiple_scattering.comp` (a workgroup per texel, an invocation per direction): only when the air changed (`sameAir`; disc scales and altitude do not count).
2. `atmosphere_sky_view.comp`, both skies, through the tables (`atmosphere_tables.glsl`) plus the sunlit ground.
3. `atmosphere_ambient.comp`: one workgroup of 256, summed in order.

`drawSky`: `fullscreen_triangle.vert` + `sky.frag`, additive, at the far depth with depth test equal (lands only where nothing drew). Adds the sky view along the camera ray (`GpuRayDebugView` push constants) plus the sun disc's radiance inside its drawn radius and the moon's, shaded by `lommelSeeliger`. Drawn:

- in the main pass after the draws, or in a pass of its own right after when the reflection surfaces are written (the sky's pipeline writes the scene image alone);
- in each reflection probe cube face (`packCubeFaceRays`) and in the planar mirror.

## In the renderer

With an atmosphere in the frame:

- **Lights through the air:** every directional light becomes `throughAtmosphere` of itself on a copy of the frame's lighting before `prioritizeLights`, so the brightest as it reaches the ground gets the cascades: after sunset, the moon.
- **`packFrameAtmosphere`:** sun and moon from `atmosphereSkyLights`; each disc's radiance is `discRadiance` of the light through the air at its drawn size. The moon's is divided by `lommelSeeligerDiscShare(moonPhaseAngle)` so its lit part sums to the moonlight; none without a sun or below `minMoonDiscShare` (1e-4), where the division would blow up.

Scene set 1 (`scene_sky.glsl`):

| Binding | With an atmosphere | Without |
|---|---|---|
| 17 | `VulkanAtmosphere`'s ambient buffer: the frame's own ambient + the sky's | `SceneBuffers::skyLight`, the frame's ambient (frames draw as before) |
| 18 | Sky view table | The 1 × 1 no-probes texture |
| 19 | `GpuAtmosphere` (`SceneBuffers::atmosphere`) | Zeros, `ground.w` 0 |

- `flatAmbient()` reads 17: the light from all around wherever probes do not say otherwise (main pass, fog, ray hits).
- `skyLightTowards(direction)`: the sky along a ray (escaping probe rays, ray-traced reflection misses); 0 without an atmosphere.

## Stars

`render/gpu/stars` (mirrored by `stars.glsl`), `VulkanStars`, `stars.vert` / `stars.frag`.

- Drawn only with an atmosphere, right after the sky in the main view: one instance of six vertices per star, no vertex buffer, additive, far depth, depth test equal.
- Each star turned by `StarrySky::toWorld`, dimmed by the transmittance table (none below the horizon), moved off screen behind the camera or inside the moon's disc.
- Light: `starLight` = brightness · 10^(−0.4 · contrast · m), times π and its colour, over the pixel's solid angle (`pixelSolidAngle`: (2|right|/width)(2|up|/height) cos³ off axis).
- Splat: a tent over a pixel either side of `starScreenPosition` (`starSplatWeight` (1 − |x|)(1 − |y|)), which sums to 1 wherever the star falls, so it never flickers.
- Records: `GpuStar` (32 bytes) in an upload buffer per frame in flight, rewritten only when that slot's catalogue pointer differs, so no frame overwrites what an earlier one still reads. Push constants: `GpuStarView` (128 bytes): the camera's rays and the equatorial rotation's rows, brightness and contrast in w.

## Tests

- `atmosphere_test`: closed-form transmittance straight up, its fall and reddening towards the horizon; medium, phase normalisation, sphere distances, table coordinates both ways, lookup, step; multiple scattering by sun height; blue noon, red sunset, symmetry, ground, ambient units, view radius, light through the air, packing, collection; moon disc shading.
- `stars_test`, `sun_test`, `celestial_test` (with the moon), `sky_object_test`.
- GPU: `atmosphere_gpu_test` (library vs mirror; real passes vs CPU: tables to half-float rounding, sky view and ambient within 1%), `stars_gpu_test`.
