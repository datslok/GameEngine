# Ambient occlusion and screen bounce light

Covers: `src/render/gpu/ambient_occlusion`, `src/render/vulkan/vulkan_ambient_occlusion`, `src/core/blue_noise`, `assets/shaders/ambient_occlusion.glsl`, `assets/shaders/ambient_occlusion.comp`, `assets/shaders/ambient_occlusion_temporal.comp`, `assets/shaders/ambient_occlusion_denoise.comp`, `assets/shaders/ambient_occlusion_downsample.comp`, `assets/shaders/ambient_occlusion_upsample.comp`

How much of the light from all around reaches a point past what lies right next to it (creases, a wall's foot, the gap under a block), and, from the same walk, the light those neighbours bounce onto it. Light probes are 0.5 m apart at best; this supplies the smaller scale. Shader maths rules: `.claude/rules/shader-mirror.md`; numbers in `ambient_occlusion_shared.h`.

## Settings

`AmbientOcclusionSettings` (`RenderSettings::ambientOcclusion`, F5 Ambient occlusion; meanings and limits in `render/gpu/ambient_occlusion.h`). Defaults worth knowing:

| Group | Fields (default) |
|---|---|
| Occlusion | `enabled` (on; off on Low), `radius` (0.5 m, about the densest probes' spacing), `falloff` (0.6), `slices`/`steps` (3/6; High 2/4, Medium 2/3), `thickness` (0.25 m) |
| Bounce light | `indirect` (on), `indirectRadius` (2 m), `indirectBrightest` (8) |
| Main pass fits | `multipleBounces`, `specularOcclusion` (on) |
| History | `temporal` (on), `historyFrames` (16), `historyDepthShare` (0.05), `antiLag` (on), `antiLagDeviations` (3), `historyClip` (2; 0 off) |
| Denoiser | `denoisePasses` (2, up to `AO_DENOISE_MAX_PASSES`), `denoisePlaneShare` (0.02) |
| Cost | `halfResolution` (on) |

## Frame order

All compute, 8 × 8 groups (`AO_GROUP_SIDE`), between the depth prepass and the contact shadows; each step timed apart:

| Step | Shader | Readout | Runs when |
|---|---|---|---|
| Colour pyramid of last frame's finished picture | (`VulkanReflections::prepareColourPyramid`) | its own | gathering; the reflections reuse it |
| Half-size depth | `ambient_occlusion_downsample.comp` | AO | `halfResolution` |
| Occlusion and gather | `ambient_occlusion.comp` | AO | always |
| Temporal accumulation | `ambient_occlusion_temporal.comp` | AOHISTORY | anti-aliasing on and `temporal` |
| Denoiser | `ambient_occlusion_denoise.comp` | AODENOISE | (no anti-aliasing, or gathering) and passes > 0 |
| Upsample | `ambient_occlusion_upsample.comp` | AOUPSAMPLE | `halfResolution` |

The main pass reads the last image written (upsampled, else denoised, else averaged, else the pass's own) at set 1 binding 25 (`AmbientOcclusionBinding`).

## The occlusion (GTAO)

Ground-truth ambient occlusion (Jimenez, Wu, Pesce, Jarabo 2016), per pixel, from the prepass's depth alone:

- **Point and normal:** the camera-relative point from the depth; the normal from `depthNormal`, which each way picks the neighbour whose next neighbour continues in line with it (`surfaceStep`), since pixels in a straight line on a plane are straight in space. Picking the nearer neighbour gives a wall's foot the wall's normal. Off-screen or sky neighbours give way.
- **Slices:** planes through the point containing the way to the camera, one screen line each, spread over half a turn and turned per pixel and frame.
- **Steps:** `steps` texels each way (`aoSampleDistance`: quadratic, staggered by noise, at least `AO_MIN_SAMPLE_PIXELS`) out to the radius on screen (`aoScreenRadius`, at most `AO_MAX_SCREEN_RADIUS`). Each sample's angle (`horizonCosine`) is pulled towards the surface's own horizon (`lowHorizonCosine`) by `occluderWeight`, so the reach has no hard edge. Off-screen and sky samples are open.
- **Closed form:** with the normal projected into the slice (`aoSlice`), `sliceVisibility` integrates the cosine-weighted open arc. Averaged over slices it is exactly 1 for any open surface, whatever its tilt.

**Noise** (`aoNoise`): three channels (slice turn, step stagger, sector rounding) from a 64 × 64 blue-noise tile (`makeBlueNoise`, void-and-cluster, three seeds in one RGBA32F texture made at start-up), repeated over the screen, each channel advanced per frame by its own irrational step (`AO_*_NOISE_FRAME_STEP`). Blue across the screen, so what averaging leaves is fine grain; independent over frames, so no sample distance stays tied to a slice angle. The frame is `taaFrame` mod 1024, 0 without anti-aliasing.

## The visibility bitmask

After Therrien, Levesque and Gilet 2023. Each slice's half circle is `AO_SECTORS` (32) bits of one `uint`:

- **Sectors of equal light,** not equal angle (`sectorShare`, from the closed form): each bit weighs the same, the cosine is in the count, and a solid horizon reproduces GTAO to within a sector.
- **Marking:** a sample marks the arc from its front to its back, `thickness` further along the view (`sectorMask`), ends rounded at a noise offset (`aoNoise`'s z) so the bits average to the arc exactly. The falloff shrinks both ends towards the surface's horizon. Light passes behind thin things, unlike a horizon.
- **Running on** (not in the paper): a sample within `thickness` in depth of its side's previous one also fills the gap between them, so walls between sparse samples are not full of holes.

## Gathered bounce light

`gathersLight` = `indirect` and last frame's picture usable (anti-aliasing on, camera known) and no probes in the frame (they already hold bounce light). It then uses `indirectRadius` and always denoises.

- Each sample adds last frame's finished picture at its point (`reprojectPixel`; nothing if it was off screen), clamped by `gatheredSampleLight` to `indirectBrightest`, times the share of its newly marked sectors (`bitCount(fresh) / 32`). Open sectors keep the light from all around, so a uniform white picture adds up to exactly the missing light (white furnace).
- Read trilinear from the colour pyramid (`VulkanColourPyramid`) at `aoSampleLevel` (log2 of the steps' spacing there), so a far sample returns the mean of the stretch it stands for, not one texel.
- **Fog taken off:** the picture is fogged, and fog glow is not light the sample sends. With a `Fog`, each sample gets `fogRemoved` (`render/gpu/volumetric_fog`, undoing dimming only down to `FOG_REMOVAL_MIN_TRANSMITTANCE`) using this frame's `fogAlongView` to the sample. The pass binds the scene's sets 0 and 1 for it (`bindSharedSets`).
- **No emitter cosine:** sectors already measure solid angle and the picture is outgoing light; a sample seen from behind leaks its visible face's colour slightly.
- **Many bounces:** the picture holds last frame's bounce, so each frame adds one (a converging series in the albedo).

## Temporal accumulation

`ambient_occlusion_temporal.comp` (mirror `aoTemporalAt`), anti-aliasing on and `temporal` set, so the upscalers get steady light rather than noise to turn into grain:

- **Average:** each texel holds the light (rgb), visibility (a), view depth and age. A new frame weighs 1 / age, the age capped at `historyFrames` (`aoHistoryAge`, `aoCurrentShare`, `accumulatedAo`): an exact mean while growing, then exponential.
- **Reprojection:** the texel's centre by the camera only (`aoPreviousPlace`), jitter not removed (each texel stands for its footprint; removing it would resample between texels every frame). Objects' own motion is not known yet (the velocity pass comes later), so movers fail the depth test and restart; the denoiser covers that.
- **Rejection** (`readAoHistory`): of four bilinear taps, only those with stored depth within `historyDepthShare` of the expected one (`aoHistoryTapKept`; sky stores 0), renormalised; the age is weighted likewise. No taps, behind last camera or off screen: age 1.
- **Anti-lag** (`aoLightChanged`): a fast mean of the gathered luminance (`aoLuminance`, `AO_ANTILAG_FAST_SHARE`) and the slow average of its square give the history's own noise. When the fast mean strays past `antiLagDeviations` of that noise (floored by `AO_ANTILAG_RELATIVE_FLOOR`, `AO_ANTILAG_ABSOLUTE_FLOOR`), the texel restarts from this frame. Tested against the history before this frame; only histories at least `AO_ANTILAG_LEAST_AGE` old.
- **History clip** (variance clipping, Salvi 2016; `aoNeighbourhood`, `clipAoHistory`, `AoClipBox`): after anti-lag, the history is held within this frame's 3 × 3 mean ± `historyClip` deviations per channel. Catches partial or slow fades anti-lag's single luminance misses; under steady noise the box is as wide as the noise.
- **Carried over** only to a frame of the same size gathering the same way with the last camera known (`AoHistoryKey`, `aoHistoryUsable`); a capture or a frame without the pass restarts it (`forgetHistory`).
- **Images:** two pairs taking turns (a pixel reads its neighbours' history, so no in-place writes): RGBA16F average and RGBA32F depth, age, fast mean and moment.

## The denoiser

`ambient_occlusion_denoise.comp` (mirror `denoiseAoAt`): an edge-aware à-trous filter (Dammertz et al. 2010, as in SVGF).

- `denoisePasses` passes of a 5 × 5 B3-spline kernel (`atrousKernelWeight`), taps 2^pass pixels apart: two passes reach about 6 pixels each way for 50 reads.
- Tap weight × `aoPlaneWeight` (distance off the pixel's surface plane over `denoisePlaneShare` of its view depth) × `aoNormalWeight` (normals' cosine to the `AO_DENOISE_NORMAL_POWER`): nothing bleeds across depth gaps or corners.
- Geometry comes from an RGBA16F image the occlusion pass writes (normal, view depth; 0 for the sky and open points, which keep their value); points rebuilt by `aoDepthRay`.
- Gathered light takes every pass; visibility only the first `AO_DENOISE_VISIBILITY_PASSES` (bounce light is soft, crease darkening is detail).
- Passes ping-pong between two images.

## Half resolution

`halfResolution`: pass, history and denoiser on a grid of half the render's width and height (`aoHalfSize`, rounded up).

- **Depth:** each texel takes one pixel of its 2 × 2 block exactly (`aoHalfSourceTexel`), the frame's pick (`aoHalfPick`: (0,0), (1,1), (1,0), (0,1), turning with the noise frame; top-left without anti-aliasing), into an R32F image. Not an average (lies on neither surface) nor the nearest (fattens thin things).
- **Nothing else changes:** the pass record carries the half grid and an inverse view-projection shifted onto the picked pixel (`aoHalfShift`, `aoShiftedInverseViewProjection`), so each texel's point is its source pixel's; the jitter in half pixels less the shift (`lens.yz`), the pick (`frame.yz`), the pyramid level bias (`lens.w`).
- **Upsample** (`aoUpsampleAt`, `VulkanAmbientOcclusion::upsampled`): each render pixel weighs its four texels by bilinear share × `aoPlaneWeight` (`aoTapWeight`), skipping open texels; below `AO_UPSAMPLE_LEAST_WEIGHT` it takes the texel nearest in view depth. Sky stays open.
- Switching remakes the images (renderer waits for the GPU) and restarts the history (the key holds the grid size).

## The pass record

`GpuAmbientOcclusionPass` (std430 storage buffer, 256 bytes, one per frame in flight; `packAmbientOcclusionPass`): layout and each vector's components in the header. Pass set 2: the record, the depth (exact texels), the picture pyramid, the blue noise, the result and the geometry image.

**Barriers:** depth from the prepass to compute read; each written image from last frame's reads to storage, discarded; images a later pass reads to compute read; the main pass's image to fragment read. Images made on first use and remade with the render size.

## In the main pass

`scene_surface.glsl`, while `ambientOcclusionOptions.x` is 1; not in the reflection probe's capture or glass:

- v = `surfaceVisibility`: min(screen, 1 + strength (baked texel − 1)), glTF's occlusion formula. The minimum, not the product: both estimate the same open share. Baked occlusion alone also applies in the capture; ray hits skip it.
- Only the light from all around (probes or flat ambient) is dimmed; direct light has its shadows.
- Diffuse: `multiBounceOcclusion(v, matte colour)` (Jimenez's cubic fit, `AO_BOUNCE_*`) while `multipleBounces` and not gathering (`ambientOcclusionOptions.y`); else v.
- Gathered rgb is added to the diffuse light from all around after the dimming.
- Specular: the reflection stand-in and the specular albedo handed to the reflection pass times `specularOcclusion(n·v, v, roughness)` (Lagarde and de Rousiers 2014), so a better reflection swapped in later stays dimmed.
- Off with no baked occlusion: every factor is exactly 1, the picture unchanged.

## Known limits

- Only what is on screen bounces; light leaves with its source.
- Specular highlights in the picture bounce as if diffuse.
- Movers' bounce trails a frame; each bounce adds a frame through the picture loop.
- The light from all around a whole room is beyond a screen search: the sky visibility grid carries it (`render/gpu/sky_visibility`).

## Tests

`ambient_occlusion_test` (closed form against quadrature, open surfaces, a wall's foot, the pass's loop over a drawn depth image, the bitmask, gathering's white furnace, accumulation, anti-lag, history clip, denoiser, half resolution), `quality_preset_test`, `gpu_timing_test`; `ambient_occlusion_gpu_test`.
