# HDR image pipeline

Covers: `src/render/gpu/tone_mapping`, `src/render/gpu/tone_map_uniforms`, `src/render/gpu/display_output`, `src/render/window_display`, `src/render/gpu/eye_adaptation`, `src/render/gpu/bloom`, `src/core/dither`, `src/render/vulkan/vulkan_bloom`, `src/render/vulkan/vulkan_eye_adaptation`, `assets/shaders/tone_map.frag`, `assets/shaders/tone_mapping.glsl`, `assets/shaders/eye_adaptation.glsl`, `assets/shaders/eye_histogram.comp`, `assets/shaders/eye_adaptation.comp`, `assets/shaders/bloom.glsl`, `assets/shaders/bloom_downsample.frag`, `assets/shaders/bloom_upsample.frag`, `assets/shaders/dither.glsl`

From the lit HDR scene to the screen: eye adaptation, bloom, night vision, tone mapping, HDR output and the dither. The camera's finish, motion blur and depth of field: `camera-effects.md`.

## The images

- `sceneColour`: the main pass's unbounded linear light, `R16G16B16A16_SFLOAT`, at the render size (upscaling), made with the depth in `ensureFrameTextures`.
- `finishedScene`: the anti-aliasing's output at the frame's size (the scene image itself with it off); kept sharp for next frame's reflections and gathered light.
- `shownScene`: what eye adaptation, bloom and tone mapping read: `finishedScene`, or depth of field's / motion blur's blurred copy while they are on.

**Order after anti-aliasing:** DOF → BLUR → EYE → BLOOM → TONE (the `GpuPass` names), then debug lines and the overlay.

## Eye adaptation (`render/gpu/eye_adaptation`, `VulkanEyeAdaptation`)

An exposure that follows the scene's brightness, as an eye adapts between noon and moonlight. Numbers in `render/gpu/eye_adaptation_shared.h`. Settings in `ToneMappingSettings` (F5 Tone mapping).

1. **Histogram** (`eye_histogram.comp`): each pixel's luminance (`pixelLuminance`, Rec. 709, as bloom) into 256 bins a tenth of a stop wide, -16 to +9.6 EV (`histogramBin`; black and darker in the first). 16 × 16 workgroups count in shared memory, then add non-empty bins with atomics: order-independent.
2. **Adaptation** (`eye_adaptation.comp`, one invocation):
   - `histogramMeanEv`: mean stops of the pixels between the darkest 50% and the brightest 10% (bins counted by their share inside), leaving out black sky and the moon, sun and bulbs.
   - `targetExposureEv`: `adaptationStrength × (log2 adaptationReference - mean)`, within `lowestExposureEv`..`highestExposureEv`.
   - `adaptExposureEv`: e^(-dt/τ) of the gap left; τ `darkAdaptationSeconds` when the exposure rises, `brightAdaptationSeconds` when it falls.
   - `holdThenAdaptExposure`: into the dark only after `darkAdaptationDelaySeconds`, counting only the time past it; a brighter frame ends the wait. A light going out reads as out, and a flickering one never sets the eye off.
3. **Result:** `GpuEyeAdaptation` (32 bytes), kept in GPU memory across frames: exposure, target, mean, the wait, and the adapted mean with its own wait (night vision's eye). `tone_map.frag` reads it the same frame (set 2 binding 2), multiplying `ToneMappingSettings::exposure` (now a bias, 1) by 2^exposure.

- No CPU round trip; one copy per frame in flight goes to read-back memory only for F4 (`exposureText`).
- Snaps to its target (`GpuEyeAdaptationStep::snap`) on its first frame, when switched back on, and on every frame a capture draws, so golden images do not depend on history.

| Setting | Default | Why |
|---|---|---|
| `eyeAdaptation` | on | |
| `adaptationReference` | 0.3 | keeps the light lab's daylight towers as tuned |
| `adaptationStrength` | 0.5 | a night five stops darker shows 2.5 darker |
| dark / delay / bright | 1 s / 1 s / 0.7 s | slower into the dark, as an eye |
| range | -4 to +1 EV | brighten dark scenes by at most a stop, so a night stays a night (the user's choice) |

## Bloom (`render/gpu/bloom`, `VulkanBloom`)

No lens focuses a point perfectly: a few percent of all light scatters into wide tails, which show as a halo round things far brighter than their surroundings. Modelled as `mix(scene, blur(scene), strength)` with the blur's weights summing to 1: light is moved, never added, with no threshold.

- **Down:** a chain of separate images (not mips, so a pass never reads what it writes), `B10G11R11_UFLOAT_PACK32` or 16-bit float where unsupported; level 0 half the frame, each next half that; `bloomLevelCount` stops before a side drops under `BLOOM_SMALLEST_LEVEL_SIDE` (4), at most `maxLevels`. `bloom_downsample.frag`: Jimenez's 13-tap filter as five weighted boxes; the Karis average (boxes by 1 / (1 + luminance)) on the first halving only, against fireflies (`fireflyReduction`).
- **Up:** `bloom_upsample.frag`, a 3 × 3 tent `radius` texels apart, added by blending onto the level above.
- **Mix:** `tone_map.frag` reads level 0 linearly (set 2 binding 1; the scene stands in without bloom), × 1 / level count (`BloomComposite`), mixed before exposure (`mixBloom`).
- Settings `BloomSettings` (`strength` 0.04, `radius` 1, `maxLevels` 6 of `BLOOM_LEVEL_LIMIT` 8). Filter weights: `render/gpu/bloom_shared.h`.
- CPU mirror (`bloomDownsample`, `bloomUpsampleAdd`, `sampleBilinear`, `bloomBlur`): `bloom_test` checks energy kept, flat images unchanged, mirror symmetry.

## Night vision (`rodLuminance`, `rodShare`, `nightVision`)

In the dark the eye sees with rods, which see no colour. Off by default (the user's choice, until physical light units).

- **Grey:** `rodLuminance` (Ward Larson et al. 1997) `Y (1.33 (1 + (Y + Z) / X) - 1.68)` from linear sRGB's XYZ, divided by white's: white keeps its brightness, blue looks brighter and red darker (the Purkinje shift). Times a tint: white at no blue shift, `NIGHT_TINT` at full, luminance 1 either way.
- **Share:** `1 - smoothstep(rodsFullEv, conesFullEv, max(pixel EV, eye EV))`, the eye's being the adapted mean (none without eye adaptation). A pixel greys only while both are dim: a lamp in a dark street and a dark corner of a sunlit room keep their colour.
- Thresholds (-9.5 and -7 EV, blue shift 0.5) sit between the light lab's moonlit field and its sunlit room, chosen by measurement since light has no physical units yet. Numbers: `render/gpu/tone_mapping_shared.h`.

## Tone mapping (`tone_map.frag`, `drawToneMap`)

`fullscreen_triangle.vert` + `tone_map.frag`; scene and bloom at set 2 bindings 0 and 1, `ToneMapUniformData` (144 bytes, `packToneMapping`) at set 3 binding 0. Per pixel, in order:

1. Read the scene (`texelFetch`; chromatic aberration reads red and blue between texels, so the scene's sampler is linear).
2. Bloom's mix, then night vision: both on the light before exposure (the receptors see the light, not the exposed image).
3. The camera finish's white balance, vignetting and grade (`camera-effects.md`).
4. × exposure (× 2^adapted), then the curve (`ToneMapCurve`) stretched to the screen's peak: `peak · curve(x / peak)`.
5. Film grain, the dither (SDR only), × the white level.

| Curve | What |
|---|---|
| `Clamp` | no curve, cut at the peak |
| `Reinhard` | x / (1 + x) |
| `Aces` | Narkowicz's fit |
| `AgX` (default) | Sobotka's, in Wrensch's minimal form: bright saturated lights roll off to white |

Mirrored by `toneMap`, `validateToneMapping`; tested in `tone_mapping_test`, `tone_map_uniforms_test`, `tone_mapping_gpu_test`.

## HDR output (`render/gpu/display_output`, `render/window_display`)

- Before acquiring each frame, `updateDisplayOutput` reads the window's HDR flag, headroom and SDR white level (`readWindowDisplay`; swapchain support asked only when HDR comes on).
- `chooseDisplayOutput` → `DisplayOutput{hdr, peakBrightness, whiteLevel}`: HDR when `DisplaySettings::hdrOutput` and the screen allow; white at `paperWhiteNits / 80` (`scrgbWhiteNits`) or Windows' level; peak at `peakNits` or the monitor's, never past the monitor's or below white, in units of white. SDR: peak `sdrPeakBrightness` 1.
- On a change, `prepareSwapchain` waits for the GPU, rebuilds the swapchain in scRGB (`R16G16B16A16_SFLOAT`, extended sRGB linear) or 8-bit sRGB, keeping the present mode (a refused HDR swapchain is logged and marked unsupported), and rebuilds the swapchain-format pipelines (`createScreenPipelines`: tone mapping, debug lines, overlay).
- Debug line colours are × the white level (`buildDebugLineVertices(lines, colourScale)`). `getDisplayOutput()` reports the choice.
- Tested in `display_output_test`.

## Dither (`dither.glsl`, `core/dither`)

- The last step on an 8-bit screen: encode to sRGB, add up to half an 8-bit step of interleaved gradient noise from the pixel, decode, so dark gradients do not band. The scene image's 16-bit floats never band, so only this step needs it.
- `dither.glsl` holds the sRGB curve and the noise, shared by several passes (`scene_surface.glsl`, fog, motion blur, the overlay). Step size in `core/dither_shared.h`.
- Tested in `dither_test`, `dither_gpu_test`.
