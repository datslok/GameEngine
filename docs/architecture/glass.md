# Glass and blended surfaces

Covers: `src/render/gpu/transparency`, `assets/shaders/transparency.glsl`, `assets/shaders/triangle_transparent.frag`, `assets/shaders/triangle_transparent_grey.frag`, `assets/shaders/glass_shadow.vert`, `assets/shaders/glass_shadow.frag`, `assets/shaders/opaque_picture.comp`, `assets/shaders/reactive_mask.comp`, `src/render/vulkan/vulkan_opaque_picture`, `src/render/vulkan/vulkan_reactive_mask`

Seen-through surfaces after the opaque scene: blending, refraction, tinted shadows and the reactive mask. Material fields and their glTF sources: [materials.md](materials.md). Shading itself: [shading.md](shading.md). Settings: `TransparencySettings` (F5 Transparency), every switch on by default.

## Two kinds of seen-through

| Kind | Material | Meaning |
|---|---|---|
| Coverage | `alphaMode` `Blend` (`MATERIAL_BLEND`) | alpha is the share of the pixel that is the surface, reflection included; for decals and fading things |
| Thin glass | `transmission` > 0 | the whole pixel is glass: its reflection stays whole (Fresnel), and of the rest the share `transmission` passes, tinted by the base colour, in place of its diffuse light; metals pass none |

An opaque material's alpha is ignored, as glTF says.

`transparentBlend(alpha, transmission, metallic, diffuse, specular, emissive, baseColour, specularAlbedo)`, per channel:

| | Formula |
|---|---|
| passing share | t = transmission × (1 − metallic) |
| surface light | (1 − t) × diffuse + specular + emissive |
| through | baseColour × (1 − specularAlbedo) × t |
| added | alpha × surface light |
| transmittance | alpha × through + (1 − alpha) |

## The glass pass

`drawTransparent` (GPU timing `Transparent`):

- **Blend:** picture = added + transmittance × picture per channel, by dual-source blending (`BlendMode::DualSourceTransmittance`, the shader's second output as the destination factor; `VulkanDevice::supportsDualSourceBlending`). Without it, `triangle_transparent_grey.frag`: alpha = 1 − mean transmittance, premultiplied.
- **When:** after the opaque scene, its reflections and the reflection probe's capture; before anti-aliasing, so glass is smoothed too. Into the scene image, depth tested (`GREATER`), not written.
- **Order:** transparent draws are kept out of every opaque view (`opaqueDraws`, `transparentDraws`), sorted far to near by bounds centre (`sortTransparentDraws`, ties stable). A double-sided one draws twice, backs then fronts, so its far side shows through its near side. Whole objects are sorted, so interpenetrating glass can misorder.
- **Shading:** `scene_surface.glsl` with `TRANSPARENT`, diffuse and specular kept apart for the blend. Its reflection: the reflection probe's prefiltered light at the roughness's level (`probeRoughnessLevel`, `fragmentReadableLight`) while one was captured this frame, else the light probes' or flat ambient's mirror light; a one-texel probe stands in when none exists. Screen ambient occlusion and mirrored lights belong to the opaque surface behind, so glass reads neither. Push constants: `GpuTransparentView`.
- **Does not:** block light in the shadow atlas (`castersInReach` skips it; it tints instead, below), stop rays, act as a mirror (`chooseMirrorPairs` skips it), appear in the reflection probe's capture, focus light (caustics), or write motion (the reactive mask covers that).

## Refraction

`TransparencySettings::refraction`. Thick glass (`thickness` > 0) and frosted glass (thin, roughness at least `TRANSPARENCY_FROSTED_ROUGHNESS` 0.05) refract (`isRefractive`, `MATERIAL_REFRACTS`): they read the picture from before the glass where their view ray lands, rather than blending.

- **The picture before the glass** (`VulkanOpaquePicture`, kept whenever glass is drawn): `opaque_picture.comp` writes level 0, the scene colour with each pixel's view distance in alpha (`viewDistanceAtPixel`, capped at 60000, under a half float's largest); `VulkanColourPyramid` (shared with screen-space reflections) builds blurrier levels, averaging alpha too. Read as set 2 binding 1.
- **Thick glass:** the view ray bends entering (`refract`, 1 / `ior`) and leaves at `solidRefractionExit` (Filament's solid sphere: the thickness × the cosine between the new direction and the inward normal), projected by `clipToScreen`. Thickness: `thickness` × its texture's green × the instance's uniform scale (`GpuInstance::handedness[1]`, `objectScale`). Absorption by Beer's law (`beerAttenuation`).
- **Frosted spread:** a facet tilted by a deviates light by about (ior − 1) a, and GGX facets tilt by about roughness², so the cone's half-angle tangent is (ior − 1) roughness² (`transmissionSpread`).
- **Scatter as gather:** each point behind the pane spreads over its own disc, radius spread × behind / (view + behind) in pixels (`frostedRadius`), up to a ceiling for the infinitely far (`frostedReach`). Each frosted pixel gathers on the depth of field's rings (`blurRingOffset`, `blurRingShare`, 81 samples) out to that ceiling, each sample at `frostedGatherLevel` with its own distance, weighed by `scatterWeight` (shared with depth of field's near layer); `frostedGatherAt` mirrors it. Near things stay sharp; a far glint's glow carries over what is near. Glass whose ceiling is under a pixel reads level 0.
- **Smooth reads:** the centre read takes the distance averaged over the blur's own area (`refractionLevel` from `pixelsPerRadian`), and each whole level through a cubic B-spline (`bsplineAxis`, `smoothPictureLevel`, Sigg and Hadwiger), blended between the two levels round it, so a bright coarse texel is a soft blob, not a square.
- **Blend:** the light through is `transparentTransmittance` at coverage 1 × absorption × the picture read, added to the glass's own light by coverage; only the uncovered share passes the picture behind, so the dual-source blend is unchanged. Smooth thin glass keeps plain blending.
- **Limits:** only what the screen shows (rays leaving it read its edge); glass behind refracting glass is missing; the one-bend sphere does not invert the picture as a real ball does.
- Push constants: `GpuTransparentView::refraction`; picture pass `GpuOpaquePicturePass`.

## Tinted shadows by rays

`TransparencySettings::glassShadows`, while ray tracing is on. Glass casts no shadow map; a shadowed light is passed through the glass between it and the surface by a ray that sees only glass (`RAY_MASK_GLASS`).

- **One face:** `glassFaceTransmittance`: 1 for a face that does not count, else `glassLightTransmittance` (the blend's `transparentTransmittance` with Schlick's Fresnel at the light's angle, since a beam meets a pane at one angle) × the thickness's `beerAttenuation`.
- **Which faces:** only those turned towards the light (`glassFaceCounts`), so a thin-box pane tints once as it is seen once; double-sided glass counts both.
- **The ray:** `glassTransmittance(from, direction, reach)` (`ray_scene.glsl`) confirms no candidate, multiplying every pane's share and stopping once nothing passes; `candidateGlassTransmittance` per candidate. Mesh structures use `NO_DUPLICATE_ANY_HIT_INVOCATION`, so each pane counts once. `lightThrough` traces the solid shadow ray first.
- **Bounds first:** `GpuGlassBounds` (set 1 binding 26, up to `TRANSPARENCY_GLASS_BOUNDS_LIMIT` 64 spheres, `packGlassBounds`): every transparent draw's bounds in the world. A segment meeting none (`segmentMeetsSphere`, `glassMayLieBetween`) is not traced; unbounded glass or overflow traces every ray. None while glass shadows are off.
- **Main pass:** while glass is in the world it draws with the glass ray pipelines (`triangle_rays.frag`, `triangle_surfaces_rays.frag`, `GLASS_SHADOW_RAYS`, the ray set as set 2). Directional lights trace to `PROBE_SHADOW_RAY_REACH`; point and spot lights to the bulb's surface (`emitterRadius` short, so a bulb's own glass does not tint it). Frames without glass use the plain pipelines. `probe_capture_rays.frag` does the same.
- Probe rays, traced reflections and mirrored lights tint by the glass on their paths too.

## Coloured shadow maps

`TransparencySettings::colouredShadowMaps`. Without the rays (ray tracing off or no ray queries), glass tints shadowed lights through maps beside the shadow atlas, while glass is in the world (`drawsColouredShadowMaps`), on GPUs with `independentBlend` (`VulkanDevice::supportsIndependentBlending`).

- **Images:** half the atlas each way (`glassShadowMapSize`, `GLASS_SHADOW_MAP_DIVISOR` 2), same tiles at the same places (`glassMapSquare`): the tint (`B10G11R11_UFLOAT`, product of every face's share) and the nearest glass depth (`R16_SFLOAT`, reversed-Z, 0 for none). 6 bytes a texel. Made the first frame that needs them (`ensureGlassShadowMaps`, which forgets the cached tiles) and when the atlas resizes; cleared white and empty; scene set 1 bindings 33 and 34.
- **Glass pass** (`drawGlassShadows`, after the solid tiles, in the Shadows timing): each redrawn tile with glass is cleared and its panes drawn (`glassShadowPipeline`, no depth target, both sides), multiplying the tint (`BlendMode::Multiply`) and keeping the larger depth (`BlendMode::Maximum`, via `PipelineDescription::extraBlends`). The light's place (`GpuShadowView::lightPlace`) decides facing as the rays do. A pane hidden from the light behind something solid is discarded (`glassHiddenFromLight`).
- **Which glass:** `glassInReach` (every transparent draw within the light's reach, whatever its `castsShadows`), then the tile's frustum. `tintedShadowSignature` adds each pane to the casters' signature (equal to `shadowCasterSignature` without glass, so other tiles stay cached). `markTileGlass` flags tiles holding glass so only their lookups read the maps.
- **Lookups:** `tileHasGlass`, `glassTintAt` (`scene_shadow_tiles.glsl`); `glassShadowTint`, `isBehindGlass` (`transparency.glsl`). A point is tinted only behind the glass; every tap of the 3 × 3 filter and the 16 soft-shadow taps is tinted at its own place, and the blocker search counts glass, so the tint's penumbra widens with distance like a shadow's. Shadow functions return colours; without glass every channel equals the old value. The fog's `shadowTap` takes it too.
- **Approximations:** one nearest glass depth, so a point between two panes takes both tints; the tint is a half-resolution wash; no focusing.
- **Cost:** the glass pass runs only when a tile with glass is redrawn (never again under a still sun).

## The reactive mask

`TransparencySettings::reactiveMask`, `reactiveScale` (1), `reactiveLimit` (0.9). Anti-aliasing reprojects by what the depth image holds, and glass is not in it, so moving glass and reflections sliding over glass would smear; the mask says how far to distrust history per pixel.

- **Measured:** `reactive_mask.comp` compares the picture after the glass pass with level 0 of the picture before (`reactiveShare`, mirrored) into an R16F image at render size (Streamline names no R8_UNORM). Both in the `Transparent` timing.
- **Share:** per channel the change over the brighter of before and after (exposure-free; `TRANSPARENCY_REACTIVE_FLOOR` keeps black at 0), the largest channel, × scale, capped at the limit. The cap stays under 1 so some history still smooths glass edges.
- **Why compared, not drawn:** dual-source blending allows one colour output, so the glass pass cannot write a mask; comparing is AMD's own approach and would cover particles too.
- **Readers:** FSR's reactive input, DLSS's current-colour bias hint, and `taa.comp` (set 0 binding 4) through `taaReactiveShare`.
- Frames without glass, or with the mask off, clear it to 0 once (skipped while it stays clear), so readers always have one.

## Tests

`transparency_test`, `material_uniforms_test`, `gltf_material_test`, `texture_encoding_test`, `gpu_scene_test` (the instance's scale), `taa_test` (`taaReactiveShare`), `culling_test` (no glass casters), `ray_tracing_test` (the glass mask), `mirrored_light_test`, `gpu_timing_test`, `light_lab_test` (the window, glass ball, frosted panel); GPU: `transparency_gpu_test`, `taa_gpu_test`.
