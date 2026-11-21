# Contact shadows

Covers: `src/render/gpu/contact_shadows`, `src/render/vulkan/vulkan_contact_shadows`, `assets/shaders/contact_shadows.glsl`, `assets/shaders/contact_shadows.comp`

Screen-space contact shadows for the sun and nearby lamps; the sun's maps: `sun-shadows.md`.

Short shadows the maps miss: map texels are millimetres to centimetres and lookups are biased sunwards, so a small object's shadow detaches or vanishes, worst at a grazing sun.

| Part | File |
|---|---|
| Maths, settings, packing, light choice | `render/gpu/contact_shadows` (mirrored by `contact_shadows.glsl`; numbers in `contact_shadows_shared.h`) |
| Pass | `contact_shadows.comp`, `render/vulkan/vulkan_contact_shadows` |
| Main pass reads | `scene_surface.glsl` (`lightContactShadow`) |

## The march

`contactShadowAt` (CPU mirror of the pass's loop), after the depth prepass, one invocation per render pixel in 8 × 8 groups (`CONTACT_GROUP_SIDE`):

- Walks from the pixel's surface towards the light in `steps` (32) even steps over `length` (0.3 m) (`contactStepDistance`).
- Start offset: the blue noise tile's green channel plus `contactNoiseTurn` (golden ratio a frame, `CONTACT_NOISE_FRAME_STEP`, only while anti-aliasing averages it).
- Each step (camera-relative) → jittered view-projection → pixel (`clipToScreen`) and view distance (clip w); the surface there → its view distance (`viewDistanceAtPixel`).
- **Crossing test** (`contactBlocked`): blocked when the step is now behind the surface by more than the bias and less than bias + a step's reach (`contactStepReach` = length / steps), and the previous step was not (the start and passed-over steps count as in front).
  - Bias: `contactBias`, `CONTACT_BIAS_PIXELS` (2) pixels' footprint at the pixel's distance.
  - Why: the depth image shows only fronts; a ray that entered something within the last step is at most one step behind its front, so deeper means passing behind it. A thin thing's shadow is at most a step's reach too wide; no fixed thickness, which doubled post shadows and widened thin ones.
- Mask = 1 − blocked × `contactDistanceFade` (`fadeStart` 40 m to `fadeEnd` 60 m) × `contactEdgeFade` (over `CONTACT_EDGE_PIXELS` 16).
- Sky pixels are lit; a march stops at the screen's edge or nearer than `CONTACT_NEAREST_W`.
- **Cut-outs pass over:** the prepass writes a solid mask (`VulkanRenderer::solidMask`, R8, cleared to 1): the solid pipeline has the attachment with colour writes off (`PipelineDescription::colourWrites`), `cut_out_prepass.frag` writes 0, cut-outs are drawn last so a solid in front stays 1. Steps landing on 0 are skipped; a cut-out's shadow map already draws its holes.

## Point and spot lights

The same pass marches towards up to `localLights` (`CONTACT_MAX_LIGHTS`, 4) lamps.

- `chooseContactLights`: seated lights holding a shadow, ranked by `lightImportance` at the focus; last frame's picks (`VulkanRenderer::contactLightHistory`) count `LightPrioritySettings::incumbentAdvantage` times as bright, so channels do not swap.
- Per pixel, `contactLightRay`: none out of range, outside a spot's cone (`CONTACT_NO_CONE` for a point light) or within the stop; else `length`, or less if the stop is nearer.
- `contactLightStop` = `emitterRadius` + `CONTACT_LIGHT_CLEARANCE` (0.25 m), so the lamp's own glowing mesh in the depth image never shadows it. A shortened march keeps its step count, so its reach shrinks with it.
- `contactShadowTowards` is the CPU mirror; `contactLightFor` builds each `ContactLight` (camera-relative).
- `ContactShadowFrame::runs()` is true for the sun or any lamp, so lamps get contact shadows at night.

## Pass data and the main pass

- `GpuContactShadowPass` (std430, 352 bytes, `packContactShadowPass`): matrices, size, sun direction with the footprint, options (length, steps, reach, noise turn), fades, per lamp position + range, beam + cone, stop, and `lightOptions` (x lamp count, y 1 when the sun marches).
- Sun mask: R32F, scene set 1 binding 31 (`ContactShadowsBinding`; a stand-in when off), multiplying directional light 0's shadow while `GpuFrameData::contactShadowOptions.x` is 1.
- Lamp mask: RGBA16F, a channel each, binding 32 (`LightContactShadowsBinding`), while `contactShadowOptions.y` is 1. A light finds its channel through `GpuFrameData::contactLightSlots` (`contactLightSlotCodes`: point slot, spot slot + `CONTACT_SPOT_SLOT_BASE`, −1 none), so light records are unchanged.
- Not read by probe captures (no depth image of their own) or glass (not in the depth image).
- Timed as `GpuPass::ContactShadows` (CONTACT in F4).

## Settings

`ContactShadowSettings` (`RenderSettings::contactShadows`, F5 Contact shadows, `engine.render.contactShadows.*`): `enabled` (off on Low), `length`, `steps` (1 to `CONTACT_MAX_STEPS` 64; more → tighter round thin things), `fadeStart`, `fadeEnd`, `localLights` (0-4; 0 = sun only). Checked by `validateContactShadows`.

## Limits

- Only what is on screen casts (an off-screen or hidden caster casts nothing).
- Only the shadowed sun and at most 4 lamps (on a street of lamps, the most important at the focus).
- A lamp seen from where it shines (the player's flashlight) casts behind what it lights, so its contact shadows are slivers.

## Tests

| Test | Checks |
|---|---|
| `contact_shadows_test`, `contact_shadows_gpu_test` | Settings and presets, steps, crossing test, bias, fades, noise turn; marches through ray-cast depths (floor and block, cut-outs passed over, a 1 cm pen at 16 and 32 steps, no halo under a hanging bar); lamp rays (range, cone, stop), choosing lights, slot codes, packing |
