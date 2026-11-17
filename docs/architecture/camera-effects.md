# Camera effects

Covers: `src/render/gpu/camera_finish`, `src/render/gpu/motion_blur`, `src/render/gpu/depth_of_field`, `src/render/vulkan/vulkan_motion_blur`, `src/render/vulkan/vulkan_depth_of_field`, `assets/shaders/camera_finish.glsl`, `assets/shaders/motion_blur.glsl`, `assets/shaders/motion_blur_tiles.comp`, `assets/shaders/motion_blur_neighbours.comp`, `assets/shaders/motion_blur.comp`, `assets/shaders/depth_of_field.glsl`, `assets/shaders/depth_of_field_pass.glsl`, `assets/shaders/depth_of_field_focus.comp`, `assets/shaders/depth_of_field_prepare.comp`, `assets/shaders/depth_of_field_neighbours.comp`, `assets/shaders/depth_of_field.comp`, `assets/shaders/depth_of_field_median.comp`, `assets/shaders/depth_of_field_composite.comp`

What a real camera and lens do to the light: the finish in the tone mapping pass, motion blur and depth of field. All settings live in `CameraSettings` (`RenderSettings::camera`, F5 Camera, keys `engine.render.camera.*`); every effect starts neutral or off (the user's choice) and costs nothing while off. Where these sit among the other post passes: `image-pipeline.md`.

## The finish (`render/gpu/camera_finish`, in `tone_map.frag`)

| Setting | Default | Effect |
|---|---|---|
| `whiteBalance`, `tint` | 6500 K, 0 | the light shown as 6500 K light: its blackbody temperature and Duv off the locus (+ greener) |
| `contrast`, `saturation` | 1, 1 | stops about mid grey × contrast; each channel's distance from luminance × saturation |
| `lift`, `gamma`, `gain` | 0, 1, 1 | ASC CDL: (light × gain + lift)^(1/gamma) |
| `vignetting` | 0 | share of a real lens's cos⁴ falloff |
| `chromaticAberration` | 0 | pixels between red and blue at the corners |
| `filmGrain` | 0 | grain strength |

- **White balance** (`whiteBalanceMatrix`, built on the CPU each frame): the chosen light's chromaticity (`planckianChromaticity`, Kim et al. 2002's fit; `whiteBalanceChromaticity` moves it `tint` across the locus in CIE 1960 uv) and 6500 K's, as Bradford cone responses; von Kries scaling by their ratio, conjugated by sRGB→XYZ. Identity when neutral. Applied to the light before the exposure.
- **Grade** (`gradeColour`): after the exposure, before the curve (then called with exposure 1): contrast per channel about `CAMERA_MID_GREY` (0.18), saturation about luminance, then gain, lift, gamma; never negative. White balance and grade run only while `gradesColour` (anything off neutral), so the neutral path is the plain curve.
- **Natural vignetting** (`naturalVignetting`): `1 / (1 + tan²θ)²`, θ off-axis from the pixel's NDC and the half field of view's tangents (`CameraView`), blended by strength; on the light before the curve.
- **Chromatic aberration** (`aberrationPlace`): red read half the shift outward along the line from the centre, blue half inward, scaled by distance over the corner's.
- **Film grain** (`filmGrain`, `grainNoise`): after the curve, before the dither: `(noise - 0.5) × strength × 4l(1 - l) × peak`, l luminance over peak (none at black or the peak); noise `pcg3d` (`hash.glsl`) of pixel and frame modulo `CAMERA_FRAME_CYCLE`.
- `packToneMapping` takes `CameraSettings` and `CameraView` and checks them (`validateCamera`). Tested in `camera_finish_test`, `camera_finish_gpu_test`.

## Motion blur (`render/gpu/motion_blur`, `VulkanMotionBlur`)

A shutter open for part of the frame smears whatever moves on the sensor: blur = screen motion over the frame × the shutter's share. McGuire et al. 2012's reconstruction filter. Numbers: `render/gpu/motion_blur_shared.h`.

| Setting | Default | Meaning |
|---|---|---|
| `motionBlur` | off | |
| `shutterAngle` | 180° | half the frame |
| `cameraMotionBlur` | on | off blurs only what moves in the world, not the world while the camera moves |

- **Needs** anti-aliasing (the velocity image) and a known last camera; while off or without them, no pass runs.
- **Where:** after the anti-aliasing and depth of field, at the output size, timed BLUR, into `blurred` (read through `shownScene`). `finishedScene` stays sharp for next frame's reflections and gathered light.

**Passes:**

1. **Tiles** (`motion_blur_tiles.comp`, `MOTION_BLUR_GROUP`² invocations per `MOTION_BLUR_TILE` (32) pixel tile): each output pixel's motion from the render texel under it, the velocity image's or, where it holds `TAA_NO_MOTION`, the camera's from depth (`cameraMotion`, `pixelMotion`; without camera blur, what the object adds to the camera's). Its half-blur in pixels × shutter share, at most a tile (`motionBlurHalfVector`), and view depth (sky at `MOTION_BLUR_SKY_DEPTH`) → an RGBA16F prepared image; the tile's longest half-blur (`largerMotion`, halved in shared memory) → an RG32F tile image.
2. **Neighbourhoods** (`motion_blur_neighbours.comp`): each tile's longest of the 3 × 3 round it, so a fast neighbour's blur reaching in is followed.
3. **Gather** (`motion_blur.comp`, mirrored by `motionBlurAt`): a neighbourhood under `MOTION_BLUR_LEAST_MOTION` (half a pixel) is copied. Otherwise the pixel, weighed by 1 / its speed, plus `MOTION_BLUR_SAMPLES` (15) samples along the neighbourhood's half-blur either way, jittered by interleaved gradient noise (`motionBlurOffset`), each by `motionBlurSampleWeight`:
   - in front (`motionBlurNearer`, soft over `MOTION_BLUR_DEPTH_SOFTNESS`) and its blur reaching the pixel (`motionBlurCone`);
   - behind and the pixel's blur reaching it;
   - or both blurred alike (`motionBlurCylinder`, counted twice).
   So a moving thing in front smears over the background, and a still thing in front keeps its edge.

**Tests** (`motion_blur_test`, `motion_blur_gpu_test`): a still picture unchanged, a sliding edge a monotonic ramp across exactly the blur's reach, a still post sharp before a moving background, smear length (half at half the shutter), symmetric samples, the packing.

## Depth of field (`render/gpu/depth_of_field`, `VulkanDepthOfField`)

A lens brings one distance to a point; others land as discs, the circle of confusion. Numbers: `render/gpu/depth_of_field_shared.h`; blur radii are in blur pixels (the half-resolution grid's, `depthOfFieldGrid`).

| Setting | Default | Meaning |
|---|---|---|
| `depthOfField` | off | |
| `fNumber` | 2.8 | 0.7 to 32 |
| `autofocus` | on | focus under the screen's centre (the crosshair) |
| `focusDistance` | 5 m | used with autofocus off |
| `focusSeconds` | 0.2 s | the focus's easing time constant |
| `showFocus` | off | tints the blur: near blue, far orange |

**The lens:**

- Thin lens on a 36 × 24 mm sensor (`DEPTH_OF_FIELD_SENSOR_HEIGHT`); focal length from the vertical field of view (`lensFocalLength`: f = 12 mm / tan(fov/2)); aperture A = f / N.
- `circleOfConfusion`: `A f / (S1 - f) · |S2 - S1| / S2`, which in inverse distances u = 1/S1, w = 1/S2 is `A f (u - w) / (1 - f u)`: linear in dioptres, so focus eases in dioptres too.
- `blurRadius`: signed (positive behind the focus, sky at w = 0) × `depthOfFieldLensScale`, at most `DEPTH_OF_FIELD_LARGEST_RADIUS` (15.5, so the 3 × 3 tiles round a pixel hold everything reaching it).
- Never focuses nearer than `DEPTH_OF_FIELD_NEAREST_FOCUS` (0.1 m) or twice the focal length (`nearestFocusInverse`).
- A game's wide lens blurs little, as a real one: the effect is subtle at typical fields of view.

**Where:** after the anti-aliasing at the output size, before motion blur (the lens forms the picture the shutter then smears), timed DOF, into `result`, read through `shownScene`.

**Passes:**

1. **Focus** (`depth_of_field_focus.comp`, one invocation): the inverse distance under the centre (or the set one), clamped, eased (`easeFocus`, exact exponential) into a 16-byte buffer kept across frames and read by later passes the same frame. Snaps when switched on, on the first frame and on every capture frame (`depthOfFieldFresh`, `capturing`).
2. **Prepare** (`depth_of_field_prepare.comp`, tiles of `DEPTH_OF_FIELD_TILE` (16) blur pixels): each blur pixel's light (mean of its 2 × 2 output pixels) and radius, the nearest of the four's (`prepareBlurTexel`: a blurred background never takes in a sharp edge's light) → RGBA16F; each tile's longest near radius → R32F.
3. **Neighbourhoods** (`depth_of_field_neighbours.comp`): each tile's longest near radius of the 3 × 3 round it.
4. **Gather** (`depth_of_field.comp`, mirrored by `depthOfFieldAt`), scatter-as-gather (Jimenez 2014): every point spreads over its own disc, so a pixel gathers out to the larger of its radius and the neighbourhood's near reach plus `DEPTH_OF_FIELD_EDGE`, counting a sample only where that sample's disc covers it (`blurCoverage`, a straight edge half a pixel each side).
   - `DEPTH_OF_FIELD_RINGS` (4) rings of 8k samples at their annuli's middles (`blurRingOffset`, odd rings half a step on), 81 with the pixel's own, each weighed by its share of the disc (`blurRingShare`).
   - **Far layer** (`farSampleWeight`): no near-field sample, none reaching farther than the pixel's own blur, so a blurred background throws no halo over a sharp thing in front; a weighted mean.
   - **Near layer** (`nearSampleWeight`): near-field samples covering the pixel, each its light spread over its disc; the summed weights (≤ 1) are its alpha.
   - A reach under `DEPTH_OF_FIELD_IN_FOCUS` copies the pixel.
5. **Median** (`depth_of_field_median.comp`, `medianOfNine`): each layer's 3 × 3 median per channel against lone bright samples; no noise, since nothing later averages over frames.
6. **Composite** (`depth_of_field_composite.comp`, `depthOfFieldComposite`): the pixel's own radius from full-resolution depth picks the sharp pixel or the far layer (all far by twice the in-focus radius), the near layer over it by its alpha, both read bilinearly; `showFocus` tints the blurred share.

`scatterWeight` is shared with frosted glass (`glass.md`).

**Tests** (`depth_of_field_test`, `depth_of_field_gpu_test`): defaults and limits, focal length, the textbook and inverse forms agreeing, nearest focus, easing, coverage, rings and shares, weights, median, prepared texel, composite; gathers over a split grid: in-focus copied, a sharp thing before a blurred background without a halo, a blurred foreground spreading over a sharp background by its discs' share; the packing; DOF's place in the timings.
