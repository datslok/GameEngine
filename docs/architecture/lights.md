# Lights

Covers: `src/scene/light.h`, `src/scene/lighting`, `src/scene/beacon`, `src/render/gpu/light_falloff`, `src/render/gpu/beacon_splat`, `src/render/gpu/beacon_splat_shared.h`, `src/render/vulkan/vulkan_beacons`, `assets/shaders/lighting.glsl`, `assets/shaders/beacon_splat.glsl`, `assets/shaders/beacon_splat.vert`, `assets/shaders/beacon_splat.frag`

The light components games place, how a frame gathers them and how the shaders receive them. Light units and the camera-relative rule: `src/render/CLAUDE.md`. How surfaces answer the light: `shading.md`. Who gets a seat and a shadow: light priority (`render/gpu/light_priority`).

## Components (`scene/light.h`)

| Component | Placed by | Notes |
|---|---|---|
| `DirectionalLight` | its `direction` (the way the light travels) | no `Transform`; `angularRadius` sets shadow softness; `reflectsSun` marks a moon (drawn phased with an `Atmosphere`) |
| `PointLight` | entity `Transform` | ends exactly at `range`; `sourceRadius` shapes the falloff, `emitterRadius` the shadow softness |
| `SpotLight` | entity `Transform` + `direction` | full inside `innerAngle`, none past `outerAngle` (half-angles, radians) |
| `AmbientLight` | none | a colour only; several add up |
| `SkyBackground` | none | lights nothing; the colour behind everything (below) |

- Light colours are 0..1 tints with an unbounded `intensity` beside them; `AmbientLight` and `SkyBackground` are plain linear colours (may pass 1).
- Point and spot lights also carry `castsShadows`, `priority` (a higher one always wins a seat or shadow) and `fast` (the probes keep its bounce in a short-memory atlas).
- Field meanings and defaults: the header.

## Falloff (`render/gpu/light_falloff`, mirrored in `lighting.glsl`)

- `distanceFalloff`: `1 / (d² + r²)` (r the `sourceRadius`) times `rangeWindow` squared, where the window is `1 - (d / range)^4` clamped to 0..1: inverse square far away, gentle up close, zero at the range with no visible edge.
- `spotCone`: smoothstep between the outer and inner cone cosines.
- Tested in `light_falloff_test`, against the GPU in `lighting_gpu_test`.

## Each frame

1. `buildFrame` → `collectLighting(world, alpha)` (`scene/lighting`) fills `FrameLighting`: ambient and background summed, point and spot positions interpolated like meshes (`PlacedPointLight`, `PlacedSpotLight`, which also carry the entity and the seat and shadow fades); the lit beacons (`PlacedBeacon`, below); the first `ProbeCascades`, `Atmosphere` and `StarrySky`; every `Fog` (its layers, in the world's order); the frame's probe events.
2. `VulkanRenderer::drawScene`: `prioritizeLights`, then `packLighting(lighting, cameraPosition)` → `GpuLights`.
3. `packLighting` drops what `canBeDrawn` refuses (a zero direction, a range that is not positive) and what is past the limits (`selectDrawableLights`; a light's place in its list is its slot, which shadow planning relies on), precomputes the cone cosines (inner clamped to outer), and measures positions from the camera, so the viewer is the shaders' origin.
4. Shadows join later (`attachShadows`, `render/gpu/shadow_map`).

## On the GPU

- One std430 record per light in three storage buffers, scene set 1 bindings 3 to 5, declared once in `scene_lights.glsl`: `GpuDirectionalLight` (64 bytes), `GpuPointLight` (48; `radiance.w` the source radius), `GpuSpotLight` (128; also an image light's window, see mirrored lights).
- The ambient and the counts go in the frame's uniform block (`GpuFrameData`, GPU scene).
- Which point and spot lights are fast stays CPU-side in `GpuLights` (`fastPoints`, `fastSpots`), for the probes only.

## Limits

| Kind | Buffer holds | Seated by default | Shadowed at most |
|---|---|---|---|
| Directional | 4 (`maxDirectionalLights`) | all | 1, the brightest (cascades) |
| Point | 1024 (`maxPointLights`) | all (`defaultPointSeats`) | 64 (`maxShadowedPointLights`), six cube tiles each |
| Spot | 256 (`maxSpotLights`) | all (`defaultSpotSeats`) | 8 (`maxShadowedSpotLights`) |

All lights fit because clustered shading makes each pixel walk only its cluster's lights; the priority mostly hands out shadows.

## Beacons (`scene/beacon`, `render/gpu/beacon_splat`)

Small, very bright lights seen from far off (obstruction lights; research `docs/research/STRUCTURES.md`, "Planned: obstruction lights").

- **Component** `Beacon` (entity `Transform`): `colour`, `intensity`, `lensRadius` (0.15 m), `flash` (`BeaconFlash`: period, on-time, ramp, phase; FAA L-864 by default, 2 s, 0.5 s, 0.05 s), `spill` and `spillRange`, `level`.
- **Flash on the CPU:** a game calls `flashBeacons(world, seconds)` every frame with its simulation clock between ticks; `beaconLevel` is a trapezium (rise and fall over the ramp), steady with no period. Each beacon's spill becomes an unshadowed, `fast` `PointLight` on its entity at spill × level (removed at 0), so the light on the walls pulses with the splat. Frame-only: nothing a tick reads.
- **Gathered:** lit ones only (level and intensity above 0), light = colour × intensity × level.
- **Drawn** (`VulkanBeacons`, BEACONS, after FOGMIX, before GLASS): one instance of six vertices each, records (`GpuBeacon`, 32 bytes, from the camera) rewritten every frame, scene sets 0-1 and the records in set 2, no push constants (the frame's uniform block gives the jittered view, its axes and size). Additive, depth tested `GREATER_OR_EQUAL` at the beacon's own depth, not written; past the far plane at the far depth.
- **Splat** (as the stars': `sky.md`): radiance = I / (d² Ω_pixel) shared by `beaconSplatWeight`: the star's tent while the lens is under a pixel, its disc (antialiased edge, over π R²) from two pixels, mixed between; sums to 1 (tent exactly, disc within 2.5 %), so the light reaching the eye is I / d² at any distance and a near lens shows I / (π r²). Fogged in the fragment shader by `fogAlongView` to its own point (the picture behind was fogged to its own depth). Not in the reactive mask yet.
- Tests: `beacon_test`, `beacon_splat_test`; GPU `beacon_splat_gpu_test`.

## The background

- `SkyBackground`'s colour (black without one) becomes `FrameLighting::background`: what the main pass clears the scene image to, so it shows wherever nothing is drawn.
- With an `Atmosphere` in the frame the sky is drawn over it (sky and atmosphere).

## The demo's lights (`games/demo/demo_game.cpp`)

A night scene that exercises every light kind:

| Light | What | Why |
|---|---|---|
| Ambient | `nightAmbient` 0.012 | linear values brighten a lot through sRGB |
| Moon | bluish shadowed `DirectionalLight`, `moonlightIntensity` 0.06; a glowing sphere 64 m from the free camera's start, `castsShadows = false` | reachable by flying (so it shows parallax); the sphere must not block its own light |
| Flashlight (F) | `Flashlight` (`gameplay/flashlight`, shared with the light lab): `flashlightBeam` in the right hand (`flashlightInHand`), or in MOBA mode held by the duck, tilted down, past its body | its shadows show beside things; the duck does not shadow its own beam |
| Lamp field (L) | 100 fast coloured point lights (`toggleLamps`), each a `Firefly` orbiting a stone pillar, bulbs not casting shadows | more lamps than the 64 point shadows, so priority chooses; moving pillars' shadows redraw every frame (a shadow stress test) |
| Floodlights (K, on at start) | four shadowed cool-white `SpotLight`s on poles at `floodlightPoleFeet()`, aimed past the course's middle so beams cross | soft spot shadows (`floodlightEmitterRadius`); tuning in the `floodlight...` constants |

- Glowing parts share `glowMaterial` (emissive, black, roughness 1).
- Light probes: one `ProbeCascades` round the camera; ground and course `isStatic`. Misses return the night ambient, so most of the probes' light is the lamps', floodlights' and flashlight's bounce.
- Each lamp-field switch is one `GeometryChange` probe event round every lamp and pillar (`signalLampField`); each floodlight switch one `LightChange` over the beams (`signalAround`).
